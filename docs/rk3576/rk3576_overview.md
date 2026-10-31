# RK3576 overview

Sample notes. Check Rockchip's official datasheet and TRM for exact figures, and replace this file with your own notes.

## CPU, GPU and memory
The RK3576 is an eight-core application processor: four Cortex-A72 big cores and four Cortex-A53 little cores.
The GPU is an Arm Mali-G52 class GPU. The board family supports LPDDR4, LPDDR4X and LPDDR5 memory.

## NPU
The RK3576 has a two-core NPU rated at about 6 TOPS. It is driven by the rknpu kernel driver.
The RKNN toolkit converts vision models, and the RKLLM toolkit converts large language models to the .rkllm format.
The model log of an RKLLM run reports npu_core_num: 2 for this chip.

## Radxa ROCK 4D
The ROCK 4D is a single-board computer built around the RK3576. It runs Debian based images from Radxa.
