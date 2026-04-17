.section .rodata.sysmodule_exefs, "a", %progbits
.balign 16
.global switch_ha_sysmodule_exefs_start
.global switch_ha_sysmodule_exefs_end
switch_ha_sysmodule_exefs_start:
.incbin "../romfs/sysmodule/exefs.nsp"
switch_ha_sysmodule_exefs_end:
.balign 16
