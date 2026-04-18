.section .rodata.sysmodule_exefs, "a", %progbits
.balign 16
.global switch_ha_sysmodule_exefs_start
.global switch_ha_sysmodule_exefs_end
switch_ha_sysmodule_exefs_start:
.incbin "../romfs/sysmodule/exefs.nsp"
switch_ha_sysmodule_exefs_end:
.balign 16

.section .rodata.overlay_loader_exefs, "a", %progbits
.balign 16
.global switch_ha_overlay_loader_exefs_start
.global switch_ha_overlay_loader_exefs_end
switch_ha_overlay_loader_exefs_start:
.incbin "../romfs/overlay-loader/exefs.nsp"
switch_ha_overlay_loader_exefs_end:
.balign 16

.section .rodata.overlay_ovl, "a", %progbits
.balign 16
.global switch_ha_overlay_ovl_start
.global switch_ha_overlay_ovl_end
switch_ha_overlay_ovl_start:
.incbin "../romfs/overlay/switch-ha-overlay.ovl"
switch_ha_overlay_ovl_end:
.balign 16
