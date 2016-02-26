#!/bin/bash

export ARCH=arm
export CROSS_COMPILE=/usr/local/tc/bin/arm-eabi-
mkdir output

make -j8 -C $(pwd) O=output apq8084_sec_defconfig VARIANT_DEFCONFIG=apq8084_sec_lentislte_skt_defconfig
make -j8 -C $(pwd) O=output

cp output/arch/arm/boot/Image $(pwd)/arch/arm/boot/zImage
