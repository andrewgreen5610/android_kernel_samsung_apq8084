#include <crypto/internal/hash.h>
#include <crypto/scatterwalk.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/wakelock.h>
#include <linux/sched.h>
#include <linux/netlink.h>
#include <linux/net.h>
#include <net/netlink.h>
#include <net/sock.h>
#include <net/net_namespace.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/audit.h>
#include <linux/jiffies.h>

#include <linux/version.h>
#include <sdp/pub_crypto_emul.h>

#define NETLINK_FIPS_CRYPTO 29
#define PUB_CRYPTO_PID_SET 3001
#define PUB_CRYPTO_RESULT 3002

#define RESULT_ARRAY_MAX_LEN 100

#define CRYPTO_MAX_TIMEOUT HZ/5

#define PUB_CRYPTO_REQ_TIMEOUT 3000

pub_crypto_control_t g_pub_crypto_control;

DEFINE_MUTEX(crypto_send_mutex);
static int user_fipscryptod_pid = 0;
static struct sock* crypto_sock = NULL;

static int pub_crypto_request_get_msg(pub_crypto_request_t *req, char **msg);
static void request_send(pub_crypto_control_t *con,
		pub_crypto_request_t *req);
static void request_wait_answer(pub_crypto_control_t *con,
		pub_crypto_request_t *req);
static pub_crypto_request_t *request_find(pub_crypto_control_t *con,
		u32 request_id);
static pub_crypto_request_t *request_alloc(u32 opcode);
static void request_free(pub_crypto_request_t *req);
static void req_dump(pub_crypto_request_t *req, const char *msg);
static void dump(unsigned char *buf, int len, const char *msg);

/* Debug */
#define PUB_CRYPTO_DEBUG		0

#if PUB_CRYPTO_DEBUG
#define PUB_CRYPTO_LOGD(FMT, ...) printk("SDP_PUB_CRYPTO[%d] : %s " FMT , current->pid, __func__, ##__VA_ARGS__)
#else
#define PUB_CRYPTO_LOGD(FMT, ...)
#endif /* PUB_CRYPTO_DEBUG */
#define PUB_CRYPTO_LOGE(FMT, ...) printk("SDP_PUB_CRYPTO[%d]  : %s " FMT , current->pid, __func__, ##__VA_ARGS__)

//static char* process_crypto_request(u8 opcode, char* send_msg,
//		int send_msg_size, int* result_len, int* ret) {
static int __do_dek_crypt(pub_crypto_request_t *req, char *ret) {
    int rc = 0;

    struct sk_buff *skb_in = NULL;
    struct sk_buff *skb_out = NULL;
    struct nlmsghdr *nlh = NULL;

	char *nl_msg = NULL;
	int nl_msg_size = 0;

	PUB_CRYPTO_LOGD("====================== \t entred\n");

	if(req == NULL) {
		PUB_CRYPTO_LOGE("invalid request\n");
		return -1;
	}

	nl_msg_size = pub_crypto_request_get_msg(req, &nl_msg);
	if(nl_msg_size <= 0) {
		PUB_CRYPTO_LOGE("invalid opcode %d\n", req->opcode);
		return -1;
	}

	// sending netlink message
	skb_in = nlmsg_new(nl_msg_size, 0);
	if (!skb_in) {
		PUB_CRYPTO_LOGE("Failed to allocate new skb: \n");
    	return -1;
	}
	
	nlh = nlmsg_put(skb_in, 0, 0, NLMSG_DONE, nl_msg_size, 0);
	NETLINK_CB(skb_in).dst_group = 0;
	memcpy(nlmsg_data(nlh), nl_msg, nl_msg_size);

	mutex_lock(&crypto_send_mutex);
	rc = nlmsg_unicast(crypto_sock, skb_in, user_fipscryptod_pid);
	mutex_unlock(&crypto_send_mutex);

	if (rc < 0) {
		PUB_CRYPTO_LOGE("Error while sending bak to user, err id: %d\n", rc);
		return -1;
	}

	/*
	 * In a very rare case, response comes before request gets into pending list.
	 */
	if(req->state != PUB_CRYPTO_REQ_FINISHED)
		request_wait_answer(&g_pub_crypto_control, req);
	else
		PUB_CRYPTO_LOGE("request already finished, skip waiting\n");

	skb_out = skb_dequeue(&crypto_sock->sk_receive_queue);

	if(req->state != PUB_CRYPTO_REQ_FINISHED) {
		PUB_CRYPTO_LOGE("FIPS_CRYPTO_ERROR!!!\n");
		/*
		 * TODO :
		 * Request not finished by an interrupt or abort.
		 */
		rc = -EINTR;
		goto out;
	}

	if(req->aborted) {
		PUB_CRYPTO_LOGE("Request aborted!!!\n");
		rc = -ETIMEDOUT;
		goto out;
	}

	if(req->result.ret < 0) {
		PUB_CRYPTO_LOGE("failed to opcode(%d)!!!\n", req->opcode);
		rc = req->result.ret;
		goto out;
	}

	switch(req->opcode) {
	case OP_DH_ENC:
	case OP_DH_DEC:
    case OP_ECDH_ENC:
    case OP_ECDH_DEC:
		dump(req->result.dek.buf, req->result.dek.len, "req->result.dek");
		memcpy(ret, &(req->result.dek), sizeof(dek_t));
		//dump(req->result.dek.buf, req->result.dek.len, "req->result.dek");
		rc = 0;
		break;
	case OP_RSA_ENC:
	case OP_RSA_DEC:
		memcpy(ret, &(req->result.dek), sizeof(dek_t));
		rc = 0;
		break;
	default:
		PUB_CRYPTO_LOGE("Not supported opcode(%d)!!!\n", req->opcode);
		rc = -EOPNOTSUPP;
		break;
	}

out:
	if(skb_out) {
		kfree_skb(skb_out);
	}
	if(rc != 0)
		req_dump(req, "request failed");

	return rc;
}

   
static int pub_crypto_recv_msg(struct sk_buff *skb, struct nlmsghdr *nlh)
{
	void			*data;
	u16			msg_type = nlh->nlmsg_type;
	u32 err = 0;
	struct audit_status	*status_get = NULL;
	u16 len = 0;

	data = NLMSG_DATA(nlh);
	len = ntohs(*(uint16_t*) (data+1));
	switch (msg_type) {
		case PUB_CRYPTO_PID_SET:
			status_get   = (struct audit_status *)data;
			user_fipscryptod_pid = status_get->pid;
			PUB_CRYPTO_LOGE("crypto_receive_msg: pid = %d\n", user_fipscryptod_pid);
			break;
		case PUB_CRYPTO_RESULT:
		{
			result_t *result = (result_t *)data;
			pub_crypto_request_t *req = NULL;

			spin_lock(&g_pub_crypto_control.lock);
			req = request_find(&g_pub_crypto_control, result->request_id);
			spin_unlock(&g_pub_crypto_control.lock);

			if(req == NULL) {
				PUB_CRYPTO_LOGE("crypto result :: error! can't find request %d\n",
						result->request_id);
#if 0
				req->state = PUB_CRYPTO_REQ_FINISHED;
				req->aborted = 1;
				wake_up(&req->waitq);

				memset(result, 0, sizeof(result_t));
#endif
			} else {
				memcpy(&req->result, result, sizeof(result_t));
				req->state = PUB_CRYPTO_REQ_FINISHED;
				wake_up(&req->waitq);

				memset(result, 0, sizeof(result_t));
			}
			break;
		}
		default:
			PUB_CRYPTO_LOGE("unknown message type : %d\n", msg_type);
			break;
	}

	return err;
}

/* Receive messages from netlink socket. */
static void crypto_recver(struct sk_buff  *skb)
{
	struct nlmsghdr *nlh;
	int len;
	int err;

	nlh = nlmsg_hdr(skb);
	len = skb->len;

	err = pub_crypto_recv_msg(skb, nlh);
}

static void dump(unsigned char *buf, int len, const char *msg) {
#if PUB_CRYPTO_DEBUG
	int i;

	printk("%s len=%d: ", msg, len);
	for(i=0;i<len;++i) {
		printk("%02x ", (unsigned char)buf[i]);
	}
	printk("\n");
#else
	printk("%s len=%d: ", msg, len);
	printk("\n");
#endif
}

int do_dek_crypt(int opcode, dek_t *in, dek_t *out, kek_t *key){
	pub_crypto_request_t *req = request_alloc(opcode);
	int ret = -1;
	req_dump(req, "request allocated");

	if(req) {
		switch(req->opcode) {
		case OP_RSA_ENC:
		case OP_RSA_DEC:
		case OP_DH_ENC:
		case OP_DH_DEC:
        case OP_ECDH_ENC:
        case OP_ECDH_DEC:
			req->cipher_param.request_id = req->id;
			req->cipher_param.opcode = req->opcode;
			memcpy(&req->cipher_param.in, (void *) in, sizeof(dek_t));
			memcpy(&req->cipher_param.key, (void *) key, sizeof(kek_t));
			break;
		default:
			PUB_CRYPTO_LOGE("opcode[%d] failed, not supported\n", opcode);
			goto error;
			break;
		}

		req_dump(req, "do_dek_crypt start");
		ret = __do_dek_crypt(req, (char *)out);
		req_dump(req, "do_dek_crypt end");

		if(ret != 0) {
			PUB_CRYPTO_LOGE("opcode[%d] failed\n", opcode);
			goto error;
		}
	} else {
		PUB_CRYPTO_LOGE("request allocation failed\n");
		return -ENOMEM;
	}

	request_free(req);
	return 0;
error:
	request_free(req);
	return -1;
}

int rsa_encryptByPub(dek_t *dek, dek_t *edek, kek_t *key){
	return do_dek_crypt(OP_RSA_ENC, dek, edek, key);
}

int rsa_decryptByPair(dek_t *edek, dek_t *dek, kek_t *key){
	return do_dek_crypt(OP_RSA_DEC, edek, dek, key);
}

int dh_encryptDEK(dek_t *dek, dek_t *edek, kek_t *key){
	return do_dek_crypt(OP_DH_ENC, dek, edek, key);
}

int dh_decryptEDEK(dek_t *edek, dek_t *dek, kek_t *key){
	return do_dek_crypt(OP_DH_DEC, edek, dek, key);
}

int ecdh_encryptDEK(dek_t *dek, dek_t *edek, kek_t *key){
    return do_dek_crypt(OP_ECDH_ENC, dek, edek, key);
}

int ecdh_decryptEDEK(dek_t *edek, dek_t *dek, kek_t *key){
    return do_dek_crypt(OP_ECDH_DEC, edek, dek, key);
}

static int pub_crypto_request_get_msg(pub_crypto_request_t *req, char **msg)
{
	int msg_len = -1;

	switch(req->opcode) {
		case OP_RSA_ENC:
		case OP_RSA_DEC:
		case OP_DH_DEC:
		case OP_DH_ENC:
        case OP_ECDH_DEC:
        case OP_ECDH_ENC:
			*msg = (char *)&req->cipher_param;
			msg_len = sizeof(cipher_param_t);
			break;
		default:
			*msg = NULL;
			msg_len = -1;
			break;
	}
	return msg_len;
}

static u32 pub_crypto_get_unique_id(pub_crypto_control_t *control)
{
	PUB_CRYPTO_LOGD("locked\n");
	spin_lock(&control->lock);

	control->reqctr++;
	/* zero is special */
	if (control->reqctr == 0)
		control->reqctr = 1;

	spin_unlock(&control->lock);
	PUB_CRYPTO_LOGD("unlocked\n");

	return control->reqctr;
}
static void req_dump(pub_crypto_request_t *req, const char *msg) {
#if PUB_CRYPTO_DEBUG
	PUB_CRYPTO_LOGD("DUMP REQUEST [%s] ID[%d] state[%d]\n", msg, req->id, req->state);
#endif
}

static void request_send(pub_crypto_control_t *con,
		pub_crypto_request_t *req) {
	spin_lock(&con->lock);
	PUB_CRYPTO_LOGD("entered, control lock\n");

	list_add_tail(&req->list, &con->pending_list);
	req->state = PUB_CRYPTO_REQ_PENDING;

	req_dump(req, "request sent");
	request_wait_answer(con, req);
	req_dump(req, "request answered");


	PUB_CRYPTO_LOGD("exit, control unlock\n");
	spin_unlock(&con->lock);
}

static void request_wait_answer(pub_crypto_control_t *con,
		pub_crypto_request_t *req)
__releases(con->lock)
__acquires(con->lock) {
	int intr;

	spin_unlock(&con->lock);
	PUB_CRYPTO_LOGD("control unlock before sleeping\n");

	while (req->state != PUB_CRYPTO_REQ_FINISHED) {
		/*
		 * TODO : can anyone answer what happens when current process gets killed here?
		 */
		intr = wait_event_interruptible_timeout(req->waitq,
				     req->state == PUB_CRYPTO_REQ_FINISHED,
				     msecs_to_jiffies(PUB_CRYPTO_REQ_TIMEOUT));
		if(req->state == PUB_CRYPTO_REQ_FINISHED)
			break;

		if(intr == 0) {
			PUB_CRYPTO_LOGE("timeout! %d [ID:%d] \n", intr, req->id);
			req->state = PUB_CRYPTO_REQ_FINISHED;
			req->aborted = 1;
			break;
		}

		if(intr == -ERESTARTSYS) {
			PUB_CRYPTO_LOGE("wait interrupted : intr %d(-ERESTARTSYS) \n", intr);
			break;
		}
	}

	PUB_CRYPTO_LOGD("control lock, process waken up\n");
	spin_lock(&con->lock);
}

static pub_crypto_request_t *request_find(pub_crypto_control_t *con,
		u32 request_id) {
	struct list_head *entry;

	list_for_each(entry, &con->pending_list) {
		 pub_crypto_request_t *req;
		req = list_entry(entry, pub_crypto_request_t, list);
		if (req->id == request_id)
			return req;
	}

	return NULL;
}

static struct kmem_cache *pub_crypto_req_cachep;

static void pub_crypto_request_init(pub_crypto_request_t *req, u32 opcode) {
	memset(req, 0, sizeof(pub_crypto_request_t));

	req->state = PUB_CRYPTO_REQ_INIT;
	req->id = pub_crypto_get_unique_id(&g_pub_crypto_control);

	INIT_LIST_HEAD(&req->list);
	init_waitqueue_head(&req->waitq);
	atomic_set(&req->count, 1);
	req->aborted = 0;
	req->opcode = opcode;
}

static pub_crypto_request_t *request_alloc(u32 opcode) {
	pub_crypto_request_t *req = kmem_cache_alloc(pub_crypto_req_cachep, GFP_KERNEL);

	if(req)
		pub_crypto_request_init(req, opcode);
	return req;
}

static void request_free(pub_crypto_request_t *req)
{
	req_dump(req, "request freed");
	list_del(&req->list);

	if(req) {
		memset(req, 0, sizeof(pub_crypto_request_t));
		kmem_cache_free(pub_crypto_req_cachep, req);
	}
}

void pub_crypto_control_init(pub_crypto_control_t *con) {
	PUB_CRYPTO_LOGD("pub_crypto_control_init");
	spin_lock_init(&con->lock);
	INIT_LIST_HEAD(&con->pending_list);
	con->reqctr = 0;
}

static int __init pub_crypto_mod_init(void) {
#if (LINUX_VERSION_CODE > KERNEL_VERSION(3,4,0))
	struct netlink_kernel_cfg cfg = {
		.input  = crypto_recver,
	};

	crypto_sock = netlink_kernel_create(&init_net, NETLINK_FIPS_CRYPTO, &cfg);
#else
	crypto_sock = netlink_kernel_create(&init_net, NETLINK_FIPS_CRYPTO, 0, crypto_recver, NULL, THIS_MODULE);
#endif

	if (!crypto_sock) {
		PUB_CRYPTO_LOGE("Failed to create Crypto Netlink Socket .. Exiting \n");
		return -ENOMEM;
	}
	PUB_CRYPTO_LOGE("netlink socket is created successfully! \n");

    pub_crypto_control_init(&g_pub_crypto_control);
    pub_crypto_req_cachep = kmem_cache_create("pub_crypto_requst",
					    sizeof(pub_crypto_request_t),
					    0, 0, NULL);
	if (!pub_crypto_req_cachep) {
    	netlink_kernel_release(crypto_sock);
		PUB_CRYPTO_LOGE("Failed to create pub_crypto_requst cache mem.. Exiting \n");
		return -ENOMEM;
	}

    return 0;
}

static void __exit pub_crypto_mod_exit(void) {
	
	/*
	if (crypto_sock && crypto_sock->sk_socket) {
        	sock_release(crypto_sock->sk_socket);
    }
    */
    netlink_kernel_release(crypto_sock);
	kmem_cache_destroy(pub_crypto_req_cachep);
}

module_init(pub_crypto_mod_init);
module_exit(pub_crypto_mod_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FIPS Crypto Algorithm");

