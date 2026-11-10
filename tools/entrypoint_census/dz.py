import sys, pefile, capstone
pe=pefile.PE(sys.argv[1],fast_load=True); img=pe.get_memory_mapped_image()
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
for a in sys.argv[2:]:
    s,e=[int(x,16) for x in a.split(':')]
    print('----',a)
    for i in md.disasm(bytes(img[s:e]),s): print('%x  %s %s'%(i.address,i.mnemonic,i.op_str))
