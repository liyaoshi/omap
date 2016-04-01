export ARCH=arm

./scripts/kconfig/merge_config.sh \
arch/arm/configs/omap2plus_defconfig \
ti_config_fragments/baseport.cfg \
ti_config_fragments/audio_display.cfg \
ti_config_fragments/ipc.cfg \
ti_config_fragments/connectivity.cfg \
ti_config_fragments/dra7_only.cfg \
android/configs/android-base.cfg \
android/configs/android-recommended.cfg \
ti_config_fragments/android_omap.cfg
