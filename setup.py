from setuptools import setup, Extension, find_packages
import numpy
import os
import commands
import string


invalid_chars = set(string.punctuation.replace("_", ""))
#print invalid_chars
source_files = ['pymcce.c']
aux_files = []
with open("lib/Makefile", "r") as makefile:
      lines = [l.strip().split() for l in makefile.readlines()]
      for line in lines:
            for word in line:
                  if word.endswith(".c") and not any(char in invalid_chars for char in word[:-2]):
                        aux_files.append("lib/" + word)
source_files.extend(aux_files)
# define the extension module
def pkgconfig(*packages, **kw):
      flag_map = {'-I': 'include_dirs', '-L': 'library_dirs', '-l': 'libraries'}
      for token in commands.getoutput("pkg-config --libs --cflags %s" % ' '.join(packages)).split():
            print token
            kw.setdefault(flag_map.get(token[:2]), []).append(token[2:])
      return kw

print pkgconfig('glib-2.0')

c_extension_module = Extension('_pymcce', 
                              sources=source_files, 
                              include_dirs=[numpy.get_include(), "/Users/kamranhaider/mcce/lib/"],
                              extra_compile_args = ["-lm", "-std=c99","-O3", "-undefined dynamic_lookup"])
"""
setup(name='pymcce',
      version='0.1',
      description='Python library for MCCE calculations',
      author='Kamran Haider',
      author_email='kamranhaider.mb@gmail.com',
      license='None',
      ext_modules=[c_extension_module],
      zip_safe=False,
      )
"""