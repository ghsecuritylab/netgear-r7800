# Copyright (c) 2013, The Linux Foundation. All rights reserved.
#
# Permission to use, copy, modify, and/or distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
# AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT
# INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
# LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
# OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
# PERFORMANCE OF THIS SOFTWARE.

NSS_CRYPTO_MOD_NAME=qca-nss-crypto

#ccflags-y += -DCONFIG_NSS_CRYPTO_DBG
ccflags-y += -DCONFIG_NSS_CRYPTO_FORCE_UNCACHE=1 -DNSS_CRYPTO_DEBUG_LEVEL=2

obj-m += $(NSS_CRYPTO_MOD_NAME).o
$(NSS_CRYPTO_MOD_NAME)-objs = nss_crypto_if.o
$(NSS_CRYPTO_MOD_NAME)-objs += nss_crypto_ctrl.o
$(NSS_CRYPTO_MOD_NAME)-objs += nss_crypto_debugfs.o

ifneq ($(findstring 3.4, $(KERNELVERSION)),)
$(NSS_CRYPTO_MOD_NAME)-objs += nss_crypto_platform.o
endif

ifneq ($(findstring 3.10, $(KERNELVERSION)),)
$(NSS_CRYPTO_MOD_NAME)-objs += nss_crypto_dtsi.o
endif

obj ?= .
ccflags-y += -I$(obj)/include -I$(obj)/
