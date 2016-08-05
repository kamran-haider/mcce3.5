#include <stdio.h>
#include "mcce.h"
#include <time.h>

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include "Python.h"
#include "numpy/arrayobject.h"

#define LEN 150


static PyObject *_pymcce_welcome(PyObject *self, PyObject *args)
{
   printf(" _________________________MCCE 3.5____________________________\n");
   printf("|                              |\n");
   printf("|    MCCE (Multi-Conformation Continuum Electrostatics)       |\n");
   printf("|   is a program developed at Marilyn Gunner's lab.       |\n");
   printf("|   MCCE is a biophysics simulation program combining     |\n");
   printf("|   continuum electrostatics and molecular mechanics.     |\n");
   printf("|   In this program, the protein side chain motions are   |\n");
   printf("|   simulated explicitly while the dielectric effect of   |\n");
   printf("|   solvent and bulk protein material is modeled by       |\n");
   printf("|   continuum electrostatics.              |\n");
   printf("|   MCCE can calculate residue pka, cofactor Em and       |\n");
   printf("|   protein PI in protein-solvent systems, and more:      |\n");
   printf("|                           |\n");
   printf("|    - Protein structural responses to changes in charge  |\n");
   printf("|    - Changes in charge state of ionizable residues due  |\n");
   printf("|      to structural changes in the protein            |\n");
   printf("|    - The structural and ionization changes caused by    |\n");
   printf("|      changes in solution pH or Eh              |\n");
   printf("|    - Find the location and stoichiometry of proton      |\n");
   printf("|      transfers coupled to electron transfer       |\n");
   printf("|    - Make side chain rotomer packing predictions as     |\n");
   printf("|      a function of pH                 |\n");
   printf("|                           |\n");
   printf("|   For questions and help, visit                         |\n");
   printf("|      https://sites.google.com/site/mccewiki/home   |\n");
   printf("|        October 2015, by MCCE Development Team      |\n");
   printf("|                           |\n");
   printf("|_________________________        ____________________________|\n");
   printf("                          MCCE 3.0                          \n\n");
   printf("Last Updates:                                              \n");
   printf("July 2016, Yifan's monte carlo no longer needs step3_out.pdb directory (fixed)\n");
   printf("July 2016, Removed PASCAL COMTE GENETIC ALGORITHM from this version\n");
   fflush(stdout);
   
   // Added by Jessica on Nov. 2015
   char buf[LEN];
   time_t curtime;
   struct tm *loc_time;

   //Getting current time of system
   curtime = time (NULL);

   // Converting current time to local time
   loc_time = localtime (&curtime);

   // Displaying date and time in standard format
   printf("%s", asctime (loc_time));
   return Py_BuildValue("i", 1);
}

/* ==== Set up the methods table ====================== */
static PyMethodDef _pymcceMethods[] = {
   {"welcome", 
   (PyCFunction)_pymcce_welcome, 
   METH_VARARGS,
   "Welcome to MCCE"
   },
   {NULL, NULL}     /* Sentinel - marks the end of this structure */

};

/* ==== Initialize the pymcce functions ====================== */
// Module name must be _pymcce in compile and linked 

void init_pymcce()  {
   (void) Py_InitModule("_pymcce", _pymcceMethods);
   import_array();  // Must be present for NumPy.  Called first after above line.
}


//static PyObject *_pymcce_welcome(PyObject *self, PyObject *args)
//static PyObject *_pymcce_initialize(PyObject *self, PyObject *args)
//static PyObject *_pymcce_energy(PyObject *self, PyObject *args)
//static PyObject *_pymcce_sample(PyObject *self, PyObject *args)

