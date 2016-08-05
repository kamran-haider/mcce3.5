from setuptools import setup, Extension, find_packages
import numpy
# define the extension module
c_extension_module = Extension('_pymcce', 
					sources=['pymcce.c'], 
					include_dirs=[numpy.get_include(), "lib"],
                              extra_compile_args = ["-O0", "-lglib-2.0", "-lgdbm",  "-lm", "-lz", "-fopenmp"],)

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
