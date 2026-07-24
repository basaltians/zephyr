# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rtp_audio_stream)

target_sources(app PRIVATE src/main.c)

if(CONFIG_SOC_FAMILY_NXP_IMXRT)
  # Relocate key networking stack components and L2 layer to ITCM
  zephyr_code_relocate(LIBRARY subsys__net__ip
    LOCATION "ITCM_TEXT" NOKEEP)
  zephyr_code_relocate(LIBRARY subsys__net
    LOCATION "ITCM_TEXT" NOKEEP)
  if(CONFIG_NET_L2_ETHERNET)
    zephyr_code_relocate(LIBRARY drivers__ethernet
      LOCATION "ITCM_TEXT" NOKEEP)
    zephyr_code_relocate(LIBRARY subsys__net__l2__ethernet
      LOCATION "ITCM_TEXT" NOKEEP)
  endif()
  if(CONFIG_RTP)
    zephyr_code_relocate(LIBRARY subsys__net__lib__rtp
      LOCATION "ITCM_TEXT" NOKEEP)
  endif()
  if(CONFIG_RTP_AUDIO_STREAM)
    zephyr_code_relocate(LIBRARY ..__aes67__subsys__audio_stream__rtp
      LOCATION "ITCM_TEXT" NOKEEP)
  endif()
endif()
