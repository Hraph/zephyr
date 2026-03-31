# Copyright (c) 2025 Core Devices LLC
# Copyright (c) 2025 SiFli Technologies(Nanjing) Co., Ltd
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(.)
zephyr_sources(soc.c)

set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/arm/cortex_m/scripts/linker.ld CACHE INTERNAL "")

# TODO: gen_ftab.py needs flash5/MPI5 (0x1C000000) for ptable, which is an LPSYS
# peripheral not in our HCPU DTS. Disabled for now — flash manually with sftool.
# Re-enable when MPI5 + flash5 partitions are added to the DTS.
#
# set(SF32LB56X_FTAB_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/../gen_ftab.py)
# set(SF32LB56X_FTAB_HEX ${PROJECT_BINARY_DIR}/sf32lb56x_ftab.hex)
# set(SF32LB56X_FTAB_GEN_COMMAND ...)
# set_property(GLOBAL APPEND PROPERTY extra_post_build_commands ...)
