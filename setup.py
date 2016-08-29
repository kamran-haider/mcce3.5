from setuptools import setup, Extension, find_packages
import numpy
import os

_SRC     =  '''ins_res.c       strip.c            del_conf.c        del_prot.c 
load_all_param.c  del_res.c load_param.c       cpy_conf.c        cpy_prot.c 
new_prot.c  cpy_res.c       get_files.c        db.c              geom.c 
free_strings.c  ins_conf.c  pdbline2atom.c     premcce.c init.c  load_pdb.c 
write_pdb.c rotamers.c      assign_rad.c       get_connect12.c   surfw.c 
vdw.c       vdw_conf.c      shuffle_n.c        cmp_conf.c        sort_conf.c 
sort_res.c  id_conf.c       energies.c         assign_crg.c      coulomb.c 
coulomb_conf.c get_vdw0.c   get_vdw1.c         relax_water.c     relax_h.c 
monte.c     monte2.c        ran2.c             relaxation.c      collect_connect.c 
torsion.c   vdw_fast.c      hbond_extra.c      swap.c            quick_e.c 
sas_native.c check_tpl.c    del_dir.c make_matrices.c'''
source_files = ['_pymcce.c']
aux_files = ["lib/"+f for f in _SRC.split()]
source_files.extend(aux_files)
print source_files
# define the extension module
c_extension_module = Extension('_pymcce', 
                              sources=source_files, 
                              include_dirs=[numpy.get_include(), "/Users/kamranhaider/mcce/lib/"],
                              extra_compile_args = ["-lm", "-std=c99","-O3", "-undefined dynamic_lookup"])
#                              extra_link_args = ["/Users/kamranhaider/mcce/lib/mcce.a"],)
# run the setup
setup(name='pymcce',
      version='0.1',
      description='Python library for MCCE calculations',
      author='Kamran Haider',
      author_email='kamranhaider.mb@gmail.com',
      license='None',
      ext_modules=[c_extension_module],
      zip_safe=False,
      )
