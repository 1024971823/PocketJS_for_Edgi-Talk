.section .rodata.pocketjs_package_edgitalk_smoke,"a",@progbits
.balign 16
.global pocketjs_package_edgitalk_smoke_start
.type pocketjs_package_edgitalk_smoke_start, @object
pocketjs_package_edgitalk_smoke_start:
.incbin "../../../pocketjs/apps/edgitalk-m55-smoke/dist/edgitalk-m55-smoke.pocket"
.global pocketjs_package_edgitalk_smoke_end
pocketjs_package_edgitalk_smoke_end:
.size pocketjs_package_edgitalk_smoke_start, pocketjs_package_edgitalk_smoke_end - pocketjs_package_edgitalk_smoke_start
