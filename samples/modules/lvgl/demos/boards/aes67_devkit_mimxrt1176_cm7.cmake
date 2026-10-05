# Copyright (c) Basalte bv
# SPDX-License-Identifier: Apache-2.0

# The devicetree overlay makes the DTCM the main RAM. This moves the frame buffers, which don't fit
# there, to the SDRAM, the code that runs on every frame from the XIP flash to the ITCM, and the
# fonts from the XIP flash to the DTCM.

if(NOT CONFIG_CODE_DATA_RELOCATION)
  return()
endif()

zephyr_code_relocate(FILES ${ZEPHYR_BASE}/drivers/display/display_mcux_elcdif.c
  FILTER "kheap_buf_display_heap"
  LOCATION "SDRAM0_NOINIT" NOKEEP)

set(kernel_dir ${ZEPHYR_BASE}/kernel)
set(arch_dir ${ZEPHYR_BASE}/arch/arm/core)
set(hal_drivers_dir ${ZEPHYR_HAL_NXP_MODULE_DIR}/mcux/mcux-sdk-ng/drivers)
zephyr_code_relocate(FILES
  ${kernel_dir}/events.c
  ${kernel_dir}/idle.c
  ${kernel_dir}/msg_q.c
  ${kernel_dir}/mutex.c
  ${kernel_dir}/poll.c
  ${kernel_dir}/queue.c
  ${kernel_dir}/sched.c
  ${kernel_dir}/scheduler.c
  ${kernel_dir}/sem.c
  ${kernel_dir}/thread.c
  ${kernel_dir}/timeout.c
  ${kernel_dir}/timer.c
  ${kernel_dir}/timeslicing.c
  ${kernel_dir}/work.c
  ${arch_dir}/cortex_m/__aeabi_read_tp.S
  ${arch_dir}/cortex_m/cpu_idle.c
  ${arch_dir}/cortex_m/exc_exit.c
  ${arch_dir}/cortex_m/isr_wrapper.c
  ${arch_dir}/cortex_m/swap_helper.S
  ${arch_dir}/mpu/arm_core_mpu.c
  ${arch_dir}/mpu/arm_mpu.c
  ${ZEPHYR_BASE}/drivers/display/display_mcux_elcdif.c
  ${ZEPHYR_BASE}/drivers/timer/cortex_m_systick.c
  ${ZEPHYR_BASE}/lib/heap/heap.c
  ${hal_drivers_dir}/cache/armv7-m7/fsl_cache.c
  ${hal_drivers_dir}/elcdif/fsl_elcdif.c
  LOCATION "ITCM_TEXT" NOKEEP)

# All of LVGL doesn't fit. Leave out the themes, the widgets the demos don't draw on every frame,
# and blending to color formats that neither the display nor the layers use.
set(lvgl_src_dir ${ZEPHYR_LVGL_MODULE_DIR}/src)
file(GLOB_RECURSE lvgl_files
  ${lvgl_src_dir}/core/*.c
  ${lvgl_src_dir}/display/*.c
  ${lvgl_src_dir}/draw/*.c
  ${lvgl_src_dir}/font/*.c
  ${lvgl_src_dir}/indev/*.c
  ${lvgl_src_dir}/layouts/*.c
  ${lvgl_src_dir}/libs/bin_decoder/*.c
  ${lvgl_src_dir}/misc/*.c
  ${lvgl_src_dir}/osal/*.c
  ${lvgl_src_dir}/stdlib/*.c
  ${lvgl_src_dir}/tick/*.c
  ${ZEPHYR_BASE}/modules/lvgl/*.c
)
list(FILTER lvgl_files EXCLUDE REGEX
  "lv_draw_sw_blend_to_(al88|i1|l8|rgb565_swapped|rgb888|argb8888_premultiplied)\\.c$")
foreach(widget arc bar button image imagebutton label line slider)
  list(APPEND lvgl_files ${lvgl_src_dir}/widgets/${widget}/lv_${widget}.c)
endforeach()
zephyr_code_relocate(FILES ${lvgl_files}
  LOCATION "ITCM_TEXT" NOKEEP)

# The glyph tables and bitmaps, read for every character drawn
file(GLOB lvgl_fonts ${lvgl_src_dir}/font/lv_font_montserrat_*.c)
zephyr_code_relocate(FILES ${lvgl_fonts}
  LOCATION "RAM_RODATA" NOKEEP)

if(CONFIG_MCUX_OPTIMIZED_MEMCPY OR CONFIG_MCUX_OPTIMIZED_MEMSET)
  # The hal memcpy() and memset() are assembly in a plain .text section, which
  # zephyr_code_relocate() skips
  zephyr_linker_sources(ITCM_SECTION ${CMAKE_CURRENT_LIST_DIR}/aes67_devkit_mimxrt1176_cm7_itcm.ld)
  target_sources(app PRIVATE ${CMAKE_CURRENT_LIST_DIR}/aes67_devkit_mimxrt1176_cm7_early_mem.c)
endif()

# if(CONFIG_LV_USE_DEMO_MUSIC)
#   file(GLOB music_assets ${ZEPHYR_LVGL_MODULE_DIR}/demos/music/assets/*.c)
#   zephyr_code_relocate(FILES ${music_assets} LOCATION "SDRAM0_4_RODATA" NOKEEP)
# endif()
