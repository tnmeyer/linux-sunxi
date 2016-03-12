#!/bin/bash
export ARCH=arm
export PATH=$PATH:/usr/local/x-tools-armv7h/x-tools7h/arm-unknown-linux-gnueabihf/bin
export CROSS_COMPILE=arm-unknown-linux-gnueabihf-
#export CROSS_COMPILE=arm-none-eabi-
export INSTALL_MOD_PATH=./modules_fw
MAKE="make"
#$MAKE menuconfig
$MAKE -j8
$MAKE uImage
$MAKE modules
rm -rf $INSTALL_MOD_PATH/lib/modules
$MAKE modules_install
