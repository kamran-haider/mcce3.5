#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include "mcce.h"

//set to skip monte carlo, only do MFE (needs pre-existing fort.38)
int MFE_ONLY = 0;

/* normal pw is garanteed to be smaller than 2000. When it is is bigger than 5000, it is 
 * represents a value refenced to a clashed pair of conformers.
 */

/* self energy in prot, prot_free, prot_fixed and copied to conflist
 * before sampling
 * pairwise in pairwise
 * counter in conflist
 */
/* This Monte Carlo Program adds entropy correction to each conformer type different by
 * proton and electron, so that the ionized and neutral will get equal chance regardless of
 * the population of conformers.
 * It has been found that the drift of calculated pKa with rotamer number is caused by this
 * entropy effect
 */
typedef struct {
    float vdw0;
    float vdw1;
    float tors;
    float ebkb;
    float dsol;
    float offset;
    float epws;
    float vpws;
    float pHpK0;
    float EhEm0;
    float TS;
    float residues;
    float total;
    float *mfePair;
    float *crg;
    float *vdw;
    float *ele;
} MFE;  // all the mfe energy terms

typedef struct  {
    int n;      /* Number of free confs on this residue */
    int on;
    int *conf;  /* idices of free confs */
    MFE mfe;   // let residue include mfe
} RESIDUE;

struct STAT {
    float a;
    float b;
    float chi2;
};
typedef struct  {
    int n;
    int *res;
} BIGLIST;
/* public variables */
PROT     prot;
RES      conflist;
float    **pairwise, **pairwise_vdw, **pairwise_ele;
float    E_base, E_state;
float    ph, eh;
int Nx;       /* number of titration points */
float *xp, *yp;   /* titration points */
RESIDUE  *free_res, *fixed_res, *all_res, *mfe_res; // mfe_res is all the mfe residue
BIGLIST  *biglist;   /* same length as free residues */
int      n_free, n_fixed, n_all, n_mfe;
FILE     *fp;
float    **occ_table;   
char **shead;


#define mev2Kcal 0.0235  // to keep consistent with mfe.py, use this constant instead of PH2KCAL/58, just for mfe part
#define TINY 1.0E-10
#define NMAX 5000
#define GET_PSUM for (j=0; j<ndim; j++) {\
                        for (sum=0.0, i=0; i<mpts; i++) sum += p[i][j];\
                         psum[j] = sum;}
#define SWAP(a,b) {swap=(a);(a)=(b);(b)=swap;}



float postrun_fround3(float x);
int   postrun_print_mfe(int i_res, float mfeP, FILE *pK_fp,  FILE *res_fp);
int   postrun_get_mfe(int i_res, int t_point);     
int   postrun_load_pairwise();
int   postrun_load_pairwise_fround3();
int   postrun_load_pairwise_vdw();
int   postrun_load_occupancy();
void  postrun_group_confs();
int   postrun_fitit();
int   postrun_load_conflist();
struct STAT postrun_fit(float a, float b);
float postrun_score(float v[]);
void postrun_dhill(float **p, float *y, int ndim, float ftol, float (*funk)(float []), int *nfunk);



int postrun()
{   
    int i;
    /* Load conformer list from FN_CONFLIST3 */
    printf("   Load conformer list from file \"%s\" ...\n", FN_CONFLIST3);
    fflush(stdout);
    prot = new_prot();
    postrun_load_conflist();
    if (conflist.n_conf == 0) {
        printf("   FATAL: error in reading conformer list %s", FN_CONFLIST3);
        return USERERR;
    }
    


    /* load pairwise */
    printf("   Load pairwise interactions ...\n"); fflush(stdout);
    if (postrun_load_pairwise()) {
        printf("   FATAL: pairwise interaction not loaded\n");
        return USERERR;
    }
    printf("   Done\n\n");
    fflush(stdout);

    occ_table = (float **) malloc(conflist.n_conf*sizeof(float *));
    for (i=0; i<conflist.n_conf; i++) occ_table[i] = (float *) malloc(env.titr_steps * sizeof(float));

    all_res = NULL;
    free_res = NULL;
    fixed_res = NULL;
    biglist  = NULL;
    

    for (i=0; i<env.titr_steps; i++) {
        
        postrun_group_confs();
    }

    printf("   Loading occupancy from fort.38 ...\n");
    if (postrun_load_occupancy()) {
        printf("   Fatal error detected in postrun_load_occupancy().\n");
        return USERERR;
    }
    printf("   Done\n\n"); fflush(stdout);
    
    
    
    /* curve fitting */
    printf("   Fit titration curves to get pKa/Em ...\n");
    fflush(stdout);
    if (postrun_fitit()) {
        printf("   Fatal error detected in fitting program.\n");
        return USERERR;
    }
    printf("   Done\n\n"); fflush(stdout);

   
    /* free pairwise */
    for (i=0; i<conflist.n_conf; i++)
        free(pairwise[i]);
    free(pairwise);
    
    printf("   Output files:\n");
    printf("      %-16s: pKa or Em from titration curve fitting.\n", CURVE_FITTING);
    printf("      %-16s: Summary of residue charges.\n", "sum_crg.out");
    
    return 0;
}

int postrun_load_conflist()
{   FILE *fp;
    char sbuff[MAXCHAR_LINE];
    char stemp[MAXCHAR_LINE];
    CONF conf_temp;
    char notfound;
    int iconf, ires;
    int kr;
    int counter;
    
    conflist.n_conf = 0;
    conflist.conf   = NULL;
       
    if (!(fp=fopen(FN_CONFLIST3, "r"))) {
        printf("   FATAL: Can't open file %s\n", FN_CONFLIST3);
        return USERERR;
    }
    fgets(sbuff, sizeof(sbuff), fp); /* skip the first line */
    counter = 0;
    while(fgets(sbuff, sizeof(sbuff), fp)) {
        /* load this line to a conf template */
        if (strlen(sbuff) < 20) continue;
        sscanf(sbuff, "%d %s %c %f %f %f %f %d %d %f %f %f %f %f %f %s", &conf_temp.iConf,
        conf_temp.uniqID,
        &conf_temp.on,
        &conf_temp.occ,
        &conf_temp.netcrg,
        &conf_temp.Em,
        &conf_temp.pKa,
        &conf_temp.e,
        &conf_temp.H,
        &conf_temp.E_vdw0,
        &conf_temp.E_vdw1,
        &conf_temp.E_tors,
        &conf_temp.E_epol,
        &conf_temp.E_dsolv,
        &conf_temp.E_extra,
        conf_temp.history);

        conf_temp.E_TS = 0.0; /* initialize entropy effect at the time of loading conflist */

        /* rescale */
        conf_temp.E_vdw0 *= env.scale_vdw0;
        conf_temp.E_vdw1 *= env.scale_vdw1;
        conf_temp.E_epol *= env.scale_ele;
        conf_temp.E_tors *= env.scale_tor;
        conf_temp.E_dsolv*= env.scale_dsolv;

        strncpy(conf_temp.resName, conf_temp.uniqID, 3); conf_temp.resName[3] = '\0';
        strncpy(conf_temp.confName, conf_temp.uniqID, 5); conf_temp.confName[5] = '\0';
        conf_temp.chainID = conf_temp.uniqID[5];
        strncpy(stemp, conf_temp.uniqID+6, 4); stemp[4] = '\0';
        conf_temp.resSeq = atoi(stemp);
        conf_temp.iCode = conf_temp.uniqID[10];
        conf_temp.n_atom = 0;
        if (conf_temp.on == 't' || conf_temp.on == 'T') conf_temp.on = 't';
        else conf_temp.on = 'f';
        conf_temp.iConf = counter;
        /* creating conflist */
        iconf = ins_conf(&conflist, conflist.n_conf, 0);
        cpy_conf(&conflist.conf[iconf], &conf_temp);
        counter++;
        
        notfound = 1;
        for (kr=0; kr<prot.n_res; kr++) {
            if (conf_temp.chainID != prot.res[kr].chainID ||
                strncmp(conf_temp.resName, prot.res[kr].resName, 3) ||
            conf_temp.resSeq != prot.res[kr].resSeq ||
            conf_temp.iCode != prot.res[kr].iCode)
            continue;
            /* residue found */
            iconf = ins_conf(&prot.res[kr], prot.res[kr].n_conf, 0);
            cpy_conf(&prot.res[kr].conf[iconf], &conf_temp);
            
            notfound = 0;
            break;
        }
        if (notfound) { /* belongs to new residue */
            ires  = ins_res(&prot, prot.n_res);
            iconf = ins_conf(&prot.res[kr], prot.res[kr].n_conf, 0);

            cpy_conf(&prot.res[kr].conf[iconf], &conf_temp);

            strncpy(prot.res[ires].resName, conf_temp.resName,3);
            prot.res[ires].chainID = conf_temp.chainID;
            prot.res[ires].resSeq = conf_temp.resSeq;
            prot.res[ires].iCode = conf_temp.iCode;
        }
    }
    return 0;
}

void postrun_group_confs()
{   int  kr, kc;
    int free_conf;
    int ir, ic;
    
    
    /* delete existing big list when regrouping */
    if (biglist != NULL) {
        for (ir=0; ir<n_free; ir++) {
            if (biglist[ir].res != NULL)
                free(biglist[ir].res);
        }
        free(biglist);
    }


    /* create all_res */
    if (all_res != NULL) {
        for (ir=0; ir<n_all; ir++) {
            if (all_res[ir].conf != NULL)
                free(all_res[ir].conf);
        }
        free(all_res);
    }
    all_res = (RESIDUE *) malloc(prot.n_res * sizeof(RESIDUE));
    n_all = prot.n_res;
    for (ir=0; ir<n_all; ir++) {
        all_res[ir].n    = prot.res[ir].n_conf;
        all_res[ir].conf = (int *) malloc(prot.res[ir].n_conf * sizeof(int));
        all_res[ir].mfe.crg = (float *) calloc(env.titr_steps, sizeof(float));
    }
    for (ir=0; ir<n_all; ir++) {
        for (ic=0; ic<all_res[ir].n; ic++) {
            all_res[ir].conf[ic] = prot.res[ir].conf[ic].iConf;
        }
    }
    
    /* create free residues */
    if (free_res != NULL) {
        for (ir=0; ir<n_free; ir++) {
            free(free_res[ir].conf);
        }
        free(free_res);
        free_res = NULL;
    }
    
    if (fixed_res != NULL) {
        for (ir=0; ir<n_fixed; ir++) {
            free(fixed_res[ir].conf);
        }
        free(fixed_res);
        fixed_res = NULL;
    }
    
    n_free = 0; n_fixed = 0;
    for (kr=0; kr<n_all; kr++) {
        free_conf = 0;
        for (kc=0; kc<all_res[kr].n; kc++) {
            if (conflist.conf[all_res[kr].conf[kc]].on == 'f') free_conf ++;
            /*
            printf("%s, %c, %f\n", conflist.conf[all_res[kr].conf[kc]].uniqID,
            conflist.conf[all_res[kr].conf[kc]].on,
            conflist.conf[all_res[kr].conf[kc]].occ);
            */
        }
        if (free_conf) {       /* free_conf stores number of free conformers in this residue */
            n_free++;
            free_res = realloc(free_res, n_free * sizeof(RESIDUE));
            free_res[n_free-1].n = free_conf;
            free_res[n_free-1].conf = (int *) malloc(free_conf*sizeof(int));
            
            ic = 0;
            for (kc=0; kc<all_res[kr].n; kc++) {
                if (conflist.conf[all_res[kr].conf[kc]].on == 'f') {
                    free_res[n_free-1].conf[ic] = conflist.conf[all_res[kr].conf[kc]].iConf;
                    ic++;
                }
                else { /* 't' must be with occ = 0.0, ignored */
                    if (fabs(conflist.conf[all_res[kr].conf[kc]].occ) > 0.000000001)  {
                        printf("   WARNING: %s has a fixed occ %.3f in a free residue\n",
                        conflist.conf[all_res[kr].conf[kc]].uniqID, conflist.conf[all_res[kr].conf[kc]].occ);
                        fflush(stdout);
                    }
                }
            }
        }
        else {
            n_fixed++;
            fixed_res = realloc(fixed_res, n_fixed * sizeof(RESIDUE));
            fixed_res[n_fixed-1].n = all_res[kr].n;
            fixed_res[n_fixed-1].conf = (int *) malloc(all_res[kr].n*sizeof(int));
            
            ic = 0;
            for (kc=0; kc<all_res[kr].n; kc++) {
                fixed_res[n_fixed-1].conf[ic] = conflist.conf[all_res[kr].conf[kc]].iConf;
                ic++;
            }
        }
    }
    
    return;
}

int postrun_load_pairwise()
{   int i, j, kc;
    EMATRIX ematrix;

    ematrix.n = 0;  
    if (load_energies(&ematrix, ".", 0)<0) {
        printf("   File %s not found\n", ENERGY_TABLE);
        return USERERR;
    }

    if (!(pairwise = (float **) malloc(ematrix.n * sizeof(float *)))) {
        printf("   FATAL: memory error in make_matrices()\n");
        return USERERR;
    }
    

    for (kc=0; kc<ematrix.n; kc++) {
        if (!(pairwise[kc] = (float *) malloc(ematrix.n * sizeof(float)))) {
            printf("   FATAL: memory error in make_matrices()\n");
            return USERERR;
        }
    }
    
    for (i=0; i<ematrix.n; i++) {
       for (j=0; j<ematrix.n; j++) {
          
           pairwise[i][j] = pairwise[j][i] = ((ematrix.pw[i][j].ele + ematrix.pw[j][i].ele)*env.scale_ele
                                            +(ematrix.pw[i][j].vdw + ematrix.pw[j][i].vdw)*env.scale_vdw)/2.0 ;
           
        }
    }

    
    /* free memory */
    free_ematrix(&ematrix);

    return 0;
}

int postrun_load_pairwise_vdw()
{
    int i, j, kc;
    EMATRIX ematrix;

    ematrix.n = 0;  
    if (load_energies(&ematrix, ".", 0)<0) {
        printf("   File %s not found\n", ENERGY_TABLE);
        return USERERR;
    }

    if (!(pairwise_vdw = (float **) malloc(ematrix.n * sizeof(float *)))) {
        printf("   FATAL: memory error in make_matrices()\n");
        return USERERR;
    }
    for (kc=0; kc<ematrix.n; kc++) {
        if (!(pairwise_vdw[kc] = (float *) malloc(ematrix.n * sizeof(float)))) {
            printf("   FATAL: memory error in make_matrices()\n");
            return USERERR;
        }
    }
    
    for (i=0; i<ematrix.n; i++) {
       for (j=0; j<ematrix.n; j++) {
           pairwise_vdw[i][j] = pairwise_vdw[j][i] = (ematrix.pw[i][j].vdw + ematrix.pw[j][i].vdw)*env.scale_vdw/2.0 ;
        }
    }
    
    /* free memory */
    free_ematrix(&ematrix);

    return 0;
}

int postrun_fitit()
{   
    int i, j;
    int N_crg;
    int N_res;
    int Counter;
    char line[20];
    struct STAT stat;   /* a structure of statistcs */
    char **head, **mhead;  // add mhead for mfe header
    float *netcrg;
    float **ypp, **ysp;
    float a, b, mid;
    float *crg, *protons, *electrons;    /* total net charge at pHs */
    float n;
    char sbuff[21];
    int n_protons, n_electrons, n_crg;
    int n_protons_grnd, n_electrons_grnd, n_crg_grnd;
    int k; // just for cycle
    int tifound; // whether do postrun_fitit
    FILE *blist_fp;
    float mfePoint;
 
    /*<<< Get titration points >>>*/
    /*--- Determine the titration points, x values ---*/
    Nx = env.titr_steps;
    
    /*--- Assign titration points to an array ---*/
    xp = (float *) malloc(Nx * sizeof(float));      /* store x points */
    yp = (float *) malloc(Nx * sizeof(float));      /* store y points */
    ypp = (float **) malloc(conflist.n_conf * sizeof(float *));
    for (i=0; i<conflist.n_conf; i++) ypp[i] = (float *) malloc(Nx * sizeof(float));
    ysp = (float **) malloc(conflist.n_conf * sizeof(float *));
    for (i=0; i<conflist.n_conf; i++) ysp[i] = (float *) malloc(Nx * sizeof(float));

    /*--- Convert x points to float ---*/
    for (i=0; i<Nx; i++) {
        if (env.titr_type == 'p') xp[i] = env.titr_ph0 + i*env.titr_phd;
        else  xp[i] = env.titr_eh0 + i*env.titr_ehd;
    }
    
    head = (char **) malloc(conflist.n_conf * sizeof(char *));
    for (i=0; i<conflist.n_conf; i++) head[i] = (char *) malloc(20 * sizeof(char));
    shead = (char **) malloc(conflist.n_conf * sizeof(char *));
    for (i=0; i<conflist.n_conf; i++) shead[i] = (char *) malloc(20 * sizeof(char));
    mhead = (char **) malloc(conflist.n_conf * sizeof(char *));
    for (i=0; i<conflist.n_conf; i++) mhead[i] = (char *) malloc(20 * sizeof(char));
    netcrg = (float *) malloc(conflist.n_conf*sizeof(float));

    
    /*<<< Group into residues >>>*/
    /* keep only charged conformers */
    Counter = 0;
    for (i=0; i <conflist.n_conf; i++) {
        if (strchr(conflist.conf[i].uniqID, '+') || strchr(conflist.conf[i].uniqID, '-')) {
            strncpy(head[Counter], conflist.conf[i].uniqID, 4); head[Counter][4] = '\0';
            strncat(head[Counter], conflist.conf[i].uniqID+5, 6);
            head[Counter][10] = '\0';
            for (j=0; j<Nx; j++) {
                ypp[Counter][j] = occ_table[i][j];
            }
            Counter++;
        }
    }
    N_crg = Counter;

    
    /* group residues */
    Counter = 0;
    strncpy(line, head[0], 10);
    for (j=0; j<Nx; j++) ysp[0][j] = ypp[0][j];
    for (i=1; i<N_crg; i++) {
        if (strncmp(head[i], line, 10)) {      /* not equal, a new residue */
            strncpy(shead[Counter], line, 10); shead[Counter][10] = '\0';
            Counter++;
            strncpy(line, head[i], 10);
            for (j=0; j<Nx; j++) ysp[Counter][j] = ypp[i][j];
        }
        else {                                 /* same residue */
            for (j=0; j<Nx; j++) ysp[Counter][j] += ypp[i][j];
        }
    }
    strncpy(shead[Counter], line, 10); shead[Counter][10] = '\0';
    N_res = Counter + 1;
    n_mfe = N_res;
    for (i=0; i<n_all; i++) {
        for (j=0; j<Nx; j++) all_res[i].mfe.crg[j] = 0.0;
    }
    // N_res are the number we do mfe
    for (i=0; i<n_mfe; i++) {
        strncpy(mhead[i], shead[i], 3); mhead[i][3] = '\0';
        strncat(mhead[i], shead[i]+4, 6); mhead[i][9] = '\0';
    }
    if (env.mfe_flag) {
        if (env.titr_type == 'p') printf("       Doing mfe at pH %.3f for all the residues\n", env.mfe_point);
        else printf("       Doing mfe at Eh %.3f for all the residues\n", env.mfe_point);
        if ((env.mfe_point>xp[0] && env.mfe_point>xp[Nx-1]) || (env.mfe_point<xp[0] && env.mfe_point<xp[Nx-1]))
            printf("         MFE: mfe point not in the titration range, do mfe at pKa or Em\n");
    }
    else printf("   MFE: didn't specify mfe point, do mfe at pKa or Em \n");
    
    /* free pairwise */
    for (i=0; i<conflist.n_conf; i++)
        free(pairwise[i]);
    free(pairwise);
   // load the pairwise interaction again, round the ele and vdw to keep consistent with mfe.py  
    if (postrun_load_pairwise_fround3()) {
        printf("   FATAL: mfe pairwise interaction not loaded\n");
        return USERERR;
    }
    // load all the conformers of mfe residues
    mfe_res = (RESIDUE *) calloc(n_mfe, sizeof(RESIDUE));
    for (k=0; k<n_mfe; k++) {
        mfe_res[k].n = 0;
        for (i=0; i<conflist.n_conf; i++) {
            strncpy(sbuff, conflist.conf[i].uniqID, 3); sbuff[3] = '\0';
            strncat(sbuff, conflist.conf[i].uniqID+5, 6); sbuff[9] = '\0';
            if (!strcmp(mhead[k], sbuff)) mfe_res[k].n++;
        }
    }
    for (i=0; i<n_mfe; i++) {
        mfe_res[i].conf = (int *) calloc(mfe_res[i].n, sizeof(int));
        mfe_res[i].mfe.mfePair = (float *) calloc(n_all, sizeof(float));
        mfe_res[i].mfe.vdw = (float *) calloc(n_all, sizeof(float));
        mfe_res[i].mfe.ele = (float *) calloc(n_all, sizeof(float));
        mfe_res[i].mfe.crg = (float *) calloc(env.titr_steps, sizeof(float));
    }

    for (i=0; i<n_mfe; i++) {
        mfe_res[i].n = 0;
        for (j=0; j<conflist.n_conf; j++) {
            strncpy(sbuff, conflist.conf[j].uniqID, 3); sbuff[3] = '\0';
            strncat(sbuff, conflist.conf[j].uniqID+5, 6); sbuff[9] = '\0';
            if (!strcmp(mhead[i], sbuff)) {
                mfe_res[i].n++;
                mfe_res[i].conf[mfe_res[i].n-1] = conflist.conf[j].iConf;
            }
        }
    }
    for (i=0; i<n_mfe; i++) { 
        for (j=0; j<mfe_res[i].n; j++) {
            conflist.conf[mfe_res[i].conf[j]].E_self0 = conflist.conf[mfe_res[i].conf[j]].E_vdw0
                                                      + conflist.conf[mfe_res[i].conf[j]].E_vdw1 
                                                      + conflist.conf[mfe_res[i].conf[j]].E_epol 
                                                      + conflist.conf[mfe_res[i].conf[j]].E_tors 
                                                      + conflist.conf[mfe_res[i].conf[j]].E_dsolv
                                                      + conflist.conf[mfe_res[i].conf[j]].E_extra;
        }
    }

    /* Write net charge ,protons, electrons to sum_crg.out, to be implemented */
    crg       = (float *) malloc(Nx * sizeof(float));
    protons   = (float *) malloc(Nx * sizeof(float));
    electrons = (float *) malloc(Nx * sizeof(float));

    memset(crg, 0, Nx * sizeof(float));
    memset(protons, 0, Nx * sizeof(float));
    memset(electrons, 0, Nx * sizeof(float));
    if ((fp = fopen("sum_crg.out", "w")) == NULL) {
        printf("   Can not open \"sum_crg.out\" to write, abort ...\n");
        return USERERR;
    }
    if (env.titr_type == 'p') {   /* pH titration */
        fprintf(fp, "  pH      ");
    }
    else {      /* Eh titration assumed */
        fprintf(fp, "  Eh      ");
    }
    for (i=0; i<Nx; i++) fprintf(fp, " %5d", (int) xp[i]);
    fprintf(fp, "\n");

    for(i=0; i<N_res; i++) {
        fprintf(fp, "%s", shead[i]);
        strncpy(sbuff, shead[i], 4); sbuff[4] = '1'; sbuff[5] = '\0';
        if (param_get( "PROTON", sbuff, "", &n_protons)) n_protons = 0;
        if (param_get( "ELECTRON", sbuff, "", &n_electrons)) n_electrons = 0;
        /* n_crg = n_protons-n_electrons; */
        n_crg = n_protons-n_electrons; 
        
        /* number of protons on ground conformer type */
        strncpy(sbuff, shead[i], 3); sbuff[3] = '0'; sbuff[4] = '1'; sbuff[5] = '\0';
        if (param_get( "PROTON", sbuff, "", &n_protons_grnd)) n_protons_grnd = 0;
        if (param_get( "ELECTRON", sbuff, "", &n_electrons_grnd)) n_electrons_grnd = 0;
        n_crg_grnd = n_protons_grnd - n_electrons_grnd;
        
        for(j=0; j<Nx; j++) {
            fprintf(fp, " %5.2f", n_crg*ysp[i][j]);
            mfe_res[i].mfe.crg[j] = n_crg*ysp[i][j] + n_crg_grnd*(1.0-ysp[i][j]);
            for (k=0; k<n_all; k++) {
                strncpy(sbuff, conflist.conf[all_res[k].conf[0]].uniqID, 3); sbuff[3] = '\0';
                strncat(sbuff, conflist.conf[all_res[k].conf[0]].uniqID+5, 6); sbuff[9] = '\0';
                // get all the charges of all the residues
                if (!strcmp(sbuff, mhead[i])) all_res[k].mfe.crg[j] = n_crg*ysp[i][j] + n_crg_grnd*(1.0-ysp[i][j]);
            }    
            crg[j] += n_crg*ysp[i][j] + n_crg_grnd*(1.0-ysp[i][j]);
            protons[j] += n_protons*ysp[i][j] + n_protons_grnd*(1.0-ysp[i][j]);
            electrons[j] += n_electrons*ysp[i][j] + n_electrons_grnd*(1.0-ysp[i][j]);
        }
        fprintf(fp, "\n");
    }

    fprintf(fp, "----------\n");

    fprintf(fp, "Net_Charge");
    for(j=0; j<Nx; j++) {
        fprintf(fp, " %5.2f", crg[j]);
    }
    fprintf(fp, "\n");

    fprintf(fp, "Protons   ");
    for(j=0; j<Nx; j++) {
        fprintf(fp, " %5.2f", protons[j]);
    }
    fprintf(fp, "\n");

    fprintf(fp, "Electrons ");
    for(j=0; j<Nx; j++) {
        fprintf(fp, " %5.2f", electrons[j]);
    }
    fprintf(fp, "\n");

    fclose(fp);


    /*<<< Loop over y values >>>*/
    if (!(fp = fopen(CURVE_FITTING, "w"))) {
        printf("   FATAL: can not write to file \"%s\".", CURVE_FITTING);
        return USERERR;
    }
    if (env.titr_type == 'p') {   /* pH titration */
        fprintf(fp, "%10s", "pH");
    }
    else {      /* Eh titration assumed */
        fprintf(fp, "%10s", "Eh");
    }
    if (env.mfe_pka){
        fprintf(fp, "    pKa/Em  n(slope) 1000*chi2      vdw0    vdw1    tors    ebkb    dsol   offset  pHpK0   EhEm0    -TS   residues   total");
    } else {
        fprintf(fp, "%10s%10s%10s", 
                "pKa/Em", 
                "n(slope)", 
                "1000*chi2");
        fprintf(fp, "%10s%10s%10s%10s%10s%10s%10s%10s%10s", 
                "offset", 
                "tors", 
                "v_s", 
                "v_b",
                "v_r",
                "dsol", 
                "e_b", 
                "e_r", 
                "total");
    }
    fprintf(fp, "\n");

    if (!(blist_fp = fopen("respair.lst", "w"))) {
        printf("can't write file respair.lst\n");
    }
    fprintf(blist_fp, " residue    partner         vdw     ele  pairwise  charge\n");



    for (i=0; i <N_res; i++) {
        /*--- Convert y points to float ---*/

        /* a reasonable guess makes optimization easier */
        a = 0.0;
        mid = 0.6;
        for (j=0; j<Nx; j++) {
            yp[j] = ysp[i][j];
            if (fabs(yp[j] - 0.5) < mid) {
                mid = fabs(yp[j] - 0.5);
                b = xp[j];
            }
        }
        tifound=1;
        if (mid >= 0.485) {
            tifound=0;
            if (fabs(yp[0]-yp[Nx-1])>0.5) {  /* jumps form <0.015 to >0.985 */
               fprintf(fp, "%s     %-25s", shead[i],"titration curve too sharp");
               mfePoint=xp[Nx/2];
            }  
            else {    // assume either all the yp >0.985 or all of them <0.015                    
                if (yp[0] > 0.985) {
                    if (strchr(shead[i], '+')) {
                        fprintf(fp, "%s     >%-24.1f", shead[i], xp[Nx-1]);
                        mfePoint=xp[Nx-1];
                    }
                    else {
                        fprintf(fp, "%s     <%-24.1f", shead[i], xp[0]);
                        mfePoint=xp[0];
                    }
                }
                else {
                    if (strchr(shead[i], '+')) {
                        fprintf(fp, "%s     <%-24.1f", shead[i], xp[0]);
                        mfePoint=xp[0];
                    }
                    else {
                        fprintf(fp, "%s     >%-24.1f", shead[i], xp[Nx-1]);
                        mfePoint=xp[Nx-1];
                    }
                }
            }            
        }
        if (tifound == 0) {
            postrun_print_mfe(i, mfePoint, fp, blist_fp);
            continue;
        }
         //   fprintf(fp, "
        /* printf("a=%.3f; b=%.3f\n",a,b); */
        stat = postrun_fit(a, b);

        if (env.titr_type == 'p') n = fabs(stat.a * 8.617342E-2 * env.monte_temp / 58.0);
        else if (env.titr_type == 'e') n = fabs(stat.a * 8.617342E-2 * env.monte_temp);
        else n = stat.a;


        //if (stat.b < xp[0] || stat.b > xp[Nx-1]) fprintf(fp, "%s        pKa or Em out of range   \n", shead[i]);
        if (stat.b < xp[0]) {
            fprintf(fp, "%s     <%-24.1f", shead[i], xp[0]);
            mfePoint = xp[0];
        }
    else if (stat.b > xp[Nx-1]) {
            fprintf(fp, "%s     >%-24.1f", shead[i], xp[Nx-1]);
            mfePoint = xp[Nx-1];
        }
        else {
            fprintf(fp, "%10s %9.3f %9.3f %9.3f", shead[i], stat.b, n, 1000*stat.chi2);
            mfePoint = stat.b;
        }
        postrun_print_mfe(i, mfePoint, fp, blist_fp); 
    }     

    free(crg);
    free(protons);
    free(electrons);
    free(xp);
    free(yp);
    for (i=0; i<conflist.n_conf; i++) free(ypp[i]);
    free(ypp);
    for (i=0; i<conflist.n_conf; i++) free(ysp[i]);
    free(ysp);
    for (i=0; i<conflist.n_conf; i++) free(head[i]);
    free(head);
    for (i=0; i<conflist.n_conf; i++) free(shead[i]);
    free(shead);
    
    return 0;
}

float postrun_fround3(float x)
{
    // round off the float point number
    char sbuff[128];
    float y;

    sprintf(sbuff, "%.3f", x);
    y=atof(sbuff);
    return y;
}

int postrun_load_pairwise_fround3()
{   int i, j, kc;
    EMATRIX ematrix;

    ematrix.n = 0;  
    if (load_energies(&ematrix, ".", 0)<0) {
        printf("   File %s not found\n", ENERGY_TABLE);
        return USERERR;
    }
    
    if (!(pairwise = (float **) malloc(ematrix.n * sizeof(float *)))) {
        printf("   FATAL: memory error in make_matrices()\n");
        return USERERR;
    } 
    pairwise_vdw = (float **) malloc(ematrix.n * sizeof(float *));
    pairwise_ele = (float **) malloc(ematrix.n * sizeof(float *));
    
    for (kc=0; kc<ematrix.n; kc++) {
        if (!(pairwise[kc] = (float *) malloc(ematrix.n * sizeof(float)))) {
            printf("   FATAL: memory error in make_matrices()\n");
            return USERERR;
        }
        if (!(pairwise_vdw[kc] = (float *) malloc(ematrix.n * sizeof(float)))) {
            printf("   FATAL: memory error in make_matrices()\n");
            return USERERR;
        }
        if (!(pairwise_ele[kc] = (float *) malloc(ematrix.n * sizeof(float)))) {
            printf("   FATAL: memory error in make_matrices()\n");
            return USERERR;
        }
    }
    
    for (i=0; i<ematrix.n; i++) {
       for (j=0; j<ematrix.n; j++) {
          // pairwise[i][j] = pairwise[j][i] = ((ematrix.pw[i][j].ele + ematrix.pw[j][i].ele)*env.scale_ele
          //                                 +(ematrix.pw[i][j].vdw + ematrix.pw[j][i].vdw)*env.scale_vdw)/2.0 ;
          // if (ematrix.pw[i][j].vdw > 998.0) pairwise[i][j] = 999.0;
            pairwise[i][j] = postrun_fround3(ematrix.pw[i][j].ele) * env.scale_ele + postrun_fround3(ematrix.pw[i][j].vdw) * env.scale_vdw;
            pairwise_vdw[i][j] = postrun_fround3(ematrix.pw[i][j].vdw) * env.scale_vdw;
            pairwise_ele[i][j] = postrun_fround3(ematrix.pw[i][j].ele) * env.scale_ele;
        }
    }

    /* free memory */
    free_ematrix(&ematrix);

    return 0;
}

int postrun_print_mfe(int i_res, float mfeP, FILE *pK_fp, FILE *res_fp)
{
    int i_low, i_high;
    int j, k;
    float cut_off, ratio;
    char head1[20], head2[20];
    RESIDUE old_mfe;

    if (env.mfe_flag) {
        if (! ((env.mfe_point>xp[0] && env.mfe_point>xp[Nx-1]) || (env.mfe_point<xp[0] && env.mfe_point<xp[Nx-1]))) mfeP = env.mfe_point;
    }
    
    cut_off = env.mfe_cutoff;
    // if (cut_off < 0.0001) cut_off = 0.5; 

    for (k=0; k<=Nx-1; k++) {
        if (xp[k] >= mfeP) {
            i_high=k; break;
        }
    }
    for (k=Nx-1; k>=0; k--) {
        if (xp[k] <= mfeP) {
            i_low=k; break;
        }
    }

    if (abs(i_high-i_low) == 0) { // do mfe at only one point
        postrun_get_mfe(i_res, i_low); 
        if (env.titr_type == 'p')
            if (env.mfe_pka){
                fprintf(pK_fp, "  %8.2f%8.2f%8.2f%8.2f%8.2f%8.2f%8.2f%8.2f%8.2f%8.2f%10.2f\n", 
                   mfe_res[i_res].mfe.vdw0/PH2KCAL, 
                   mfe_res[i_res].mfe.vdw1/PH2KCAL, 
                   mfe_res[i_res].mfe.tors/PH2KCAL, 
                   mfe_res[i_res].mfe.ebkb/PH2KCAL, 
                   mfe_res[i_res].mfe.dsol/PH2KCAL, 
                   mfe_res[i_res].mfe.offset/PH2KCAL, 
                   mfe_res[i_res].mfe.pHpK0/PH2KCAL, 
                   mfe_res[i_res].mfe.EhEm0/PH2KCAL, 
                   mfe_res[i_res].mfe.TS/PH2KCAL, 
                   mfe_res[i_res].mfe.residues/PH2KCAL, 
                   mfe_res[i_res].mfe.total/PH2KCAL);
            } else {
                fprintf(pK_fp, "%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f\n",
                    mfe_res[i_res].mfe.offset/PH2KCAL,
                    mfe_res[i_res].mfe.tors/PH2KCAL,
                    mfe_res[i_res].mfe.vdw0/PH2KCAL, 
                    mfe_res[i_res].mfe.vdw1/PH2KCAL,
                    mfe_res[i_res].mfe.vpws/PH2KCAL,
                    mfe_res[i_res].mfe.dsol/PH2KCAL, 
                    mfe_res[i_res].mfe.ebkb/PH2KCAL,
                    mfe_res[i_res].mfe.epws/PH2KCAL,
                    mfe_res[i_res].mfe.total/PH2KCAL);
            }        
        else 
            if (env.mfe_pka){
                fprintf(pK_fp, "  %8.1f%8.1f%8.1f%8.1f%8.1f%8.1f%8.1f%8.1f%8.1f%8.1f%10.1f\n", 
                   mfe_res[i_res].mfe.vdw0/mev2Kcal, 
                   mfe_res[i_res].mfe.vdw1/mev2Kcal, 
                   mfe_res[i_res].mfe.tors/mev2Kcal, 
                   mfe_res[i_res].mfe.ebkb/mev2Kcal, 
                   mfe_res[i_res].mfe.dsol/mev2Kcal, 
                   mfe_res[i_res].mfe.offset/mev2Kcal, 
                   mfe_res[i_res].mfe.pHpK0/mev2Kcal, 
                   mfe_res[i_res].mfe.EhEm0/mev2Kcal, 
                   mfe_res[i_res].mfe.TS/mev2Kcal, 
                   mfe_res[i_res].mfe.residues/mev2Kcal, 
                   mfe_res[i_res].mfe.total/mev2Kcal);
            }else {
                fprintf(pK_fp, "%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f\n",
                       mfe_res[i_res].mfe.offset/PH2KCAL,
                       mfe_res[i_res].mfe.tors/PH2KCAL,
                       mfe_res[i_res].mfe.vdw0/PH2KCAL, 
                       mfe_res[i_res].mfe.vdw1/PH2KCAL,
                       mfe_res[i_res].mfe.vpws/PH2KCAL,
                       mfe_res[i_res].mfe.dsol/PH2KCAL, 
                       mfe_res[i_res].mfe.ebkb/PH2KCAL,
                       mfe_res[i_res].mfe.epws/PH2KCAL,
                       mfe_res[i_res].mfe.total/PH2KCAL);
            }
        for (j=0; j<n_all; j++) {
            head1[0]='\0'; head2[0]='\0';
            if (fabs(mfe_res[i_res].mfe.mfePair[j]) < cut_off) continue;
            strcpy(head1, shead[i_res]);
            strncpy(head2, conflist.conf[all_res[j].conf[0]].uniqID, 3); head2[3] = '\0';
            strncat(head2, conflist.conf[all_res[j].conf[0]].uniqID+5, 6); head2[9] = '\0';
            if (env.titr_type == 'p') 
                fprintf(res_fp, "%s  %s   %8.2f%8.2f%8.2f%8.2f\n", head1, head2, 
                mfe_res[i_res].mfe.vdw[j]/PH2KCAL, mfe_res[i_res].mfe.ele[j]/PH2KCAL, mfe_res[i_res].mfe.mfePair[j]/PH2KCAL, all_res[j].mfe.crg[i_low]);
            else
                fprintf(res_fp, "%s  %s   %8.2f%8.2f%8.2f%8.2f\n", head1, head2, 
                mfe_res[i_res].mfe.vdw[j]/mev2Kcal, mfe_res[i_res].mfe.ele[j]/mev2Kcal, mfe_res[i_res].mfe.mfePair[j]/mev2Kcal, all_res[j].mfe.crg[i_low]);
        }
    }
    else {    /* get mfe from two titration points */
        ratio = (mfeP - xp[i_low])/(xp[i_high]-xp[i_low]);
        postrun_get_mfe(i_res, i_low);
        old_mfe.n = mfe_res[i_res].n;
        old_mfe.conf = (int *) calloc(old_mfe.n, sizeof(int));
        old_mfe.mfe.mfePair = (float *) calloc(n_all, sizeof(float));
        old_mfe.mfe.vdw = (float *) calloc(n_all, sizeof(float));
        old_mfe.mfe.ele = (float *) calloc(n_all, sizeof(float));
        old_mfe.mfe.vdw0 = mfe_res[i_res].mfe.vdw0;
        old_mfe.mfe.vdw1 = mfe_res[i_res].mfe.vdw1;
        old_mfe.mfe.ebkb = mfe_res[i_res].mfe.ebkb;
        old_mfe.mfe.tors = mfe_res[i_res].mfe.tors;
        old_mfe.mfe.dsol = mfe_res[i_res].mfe.dsol;
        old_mfe.mfe.offset = mfe_res[i_res].mfe.offset;
        old_mfe.mfe.pHpK0 = mfe_res[i_res].mfe.pHpK0;
        old_mfe.mfe.EhEm0 = mfe_res[i_res].mfe.EhEm0;
        old_mfe.mfe.TS = mfe_res[i_res].mfe.TS;
        old_mfe.mfe.residues = mfe_res[i_res].mfe.residues;
        old_mfe.mfe.total = mfe_res[i_res].mfe.total;

        for (k=0; k<n_all; k++) {
            old_mfe.mfe.mfePair[k] = mfe_res[i_res].mfe.mfePair[k];                       
            old_mfe.mfe.vdw[k] = mfe_res[i_res].mfe.vdw[k];                       
            old_mfe.mfe.ele[k] = mfe_res[i_res].mfe.ele[k];                       
        }
        old_mfe.mfe.vpws = mfe_res[i_res].mfe.vpws;
        old_mfe.mfe.epws = mfe_res[i_res].mfe.epws;

        postrun_get_mfe(i_res, i_high);
        mfe_res[i_res].mfe.vdw0 = old_mfe.mfe.vdw0*(1-ratio) + mfe_res[i_res].mfe.vdw0*ratio;
        mfe_res[i_res].mfe.vdw1 = old_mfe.mfe.vdw1*(1-ratio) + mfe_res[i_res].mfe.vdw1*ratio;
        mfe_res[i_res].mfe.ebkb = old_mfe.mfe.ebkb*(1-ratio) + mfe_res[i_res].mfe.ebkb*ratio;
        mfe_res[i_res].mfe.tors = old_mfe.mfe.tors*(1-ratio) + mfe_res[i_res].mfe.tors*ratio;
        mfe_res[i_res].mfe.dsol = old_mfe.mfe.dsol*(1-ratio) + mfe_res[i_res].mfe.dsol*ratio;
        mfe_res[i_res].mfe.offset = old_mfe.mfe.offset*(1-ratio) + mfe_res[i_res].mfe.offset*ratio;
        mfe_res[i_res].mfe.pHpK0 = old_mfe.mfe.pHpK0*(1-ratio) + mfe_res[i_res].mfe.pHpK0*ratio;
        mfe_res[i_res].mfe.EhEm0 = old_mfe.mfe.EhEm0*(1-ratio) + mfe_res[i_res].mfe.EhEm0*ratio;
        mfe_res[i_res].mfe.TS = old_mfe.mfe.TS*(1-ratio) + mfe_res[i_res].mfe.TS*ratio;
        mfe_res[i_res].mfe.residues = old_mfe.mfe.residues*(1-ratio) + mfe_res[i_res].mfe.residues*ratio;
        mfe_res[i_res].mfe.total = old_mfe.mfe.total*(1-ratio) + mfe_res[i_res].mfe.total*ratio;

        for (k=0; k<n_all; k++) {
            mfe_res[i_res].mfe.mfePair[k] = old_mfe.mfe.mfePair[k]*(1-ratio) + mfe_res[i_res].mfe.mfePair[k]*ratio;
            mfe_res[i_res].mfe.vdw[k] = old_mfe.mfe.vdw[k]*(1-ratio) + mfe_res[i_res].mfe.vdw[k]*ratio;
            mfe_res[i_res].mfe.ele[k] = old_mfe.mfe.ele[k]*(1-ratio) + mfe_res[i_res].mfe.ele[k]*ratio;
        }
        mfe_res[i_res].mfe.vpws = old_mfe.mfe.vpws*(1-ratio) + mfe_res[i_res].mfe.vpws*ratio;
        mfe_res[i_res].mfe.epws = old_mfe.mfe.epws*(1-ratio) + mfe_res[i_res].mfe.epws*ratio;
        
        if (env.titr_type == 'p') 
            if (env.mfe_pka){
                fprintf(pK_fp, "  %8.2f%8.2f%8.2f%8.2f%8.2f%8.2f%8.2f%8.2f%8.2f%8.2f%10.2f\n", 
                   mfe_res[i_res].mfe.vdw0/PH2KCAL, 
                   mfe_res[i_res].mfe.vdw1/PH2KCAL, 
                   mfe_res[i_res].mfe.tors/PH2KCAL, 
                   mfe_res[i_res].mfe.ebkb/PH2KCAL, 
                   mfe_res[i_res].mfe.dsol/PH2KCAL, 
                   mfe_res[i_res].mfe.offset/PH2KCAL, 
                   mfe_res[i_res].mfe.pHpK0/PH2KCAL, 
                   mfe_res[i_res].mfe.EhEm0/PH2KCAL, 
                   mfe_res[i_res].mfe.TS/PH2KCAL, 
                   mfe_res[i_res].mfe.residues/PH2KCAL, 
                   mfe_res[i_res].mfe.total/PH2KCAL);
            }else {
                fprintf(pK_fp, "%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f\n",
                       mfe_res[i_res].mfe.offset/PH2KCAL,
                       mfe_res[i_res].mfe.tors/PH2KCAL,
                       mfe_res[i_res].mfe.vdw0/PH2KCAL, 
                       mfe_res[i_res].mfe.vdw1/PH2KCAL,
                       mfe_res[i_res].mfe.vpws/PH2KCAL,
                       mfe_res[i_res].mfe.dsol/PH2KCAL, 
                       mfe_res[i_res].mfe.ebkb/PH2KCAL,
                       mfe_res[i_res].mfe.epws/PH2KCAL,
                       mfe_res[i_res].mfe.total/PH2KCAL);
            }   
        else 
            if (env.mfe_pka){
                fprintf(pK_fp, "  %8.1f%8.1f%8.1f%8.1f%8.1f%8.1f%8.1f%8.1f%8.1f%8.1f%10.1f\n", 
                   mfe_res[i_res].mfe.vdw0/mev2Kcal, 
                   mfe_res[i_res].mfe.vdw1/mev2Kcal, 
                   mfe_res[i_res].mfe.tors/mev2Kcal, 
                   mfe_res[i_res].mfe.ebkb/mev2Kcal, 
                   mfe_res[i_res].mfe.dsol/mev2Kcal, 
                   mfe_res[i_res].mfe.offset/mev2Kcal, 
                   mfe_res[i_res].mfe.pHpK0/mev2Kcal, 
                   mfe_res[i_res].mfe.EhEm0/mev2Kcal, 
                   mfe_res[i_res].mfe.TS/mev2Kcal, 
                   mfe_res[i_res].mfe.residues/mev2Kcal, 
                   mfe_res[i_res].mfe.total/mev2Kcal);
            }else {
                fprintf(pK_fp, "%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f%10.2f\n",
                       mfe_res[i_res].mfe.offset/PH2KCAL,
                       mfe_res[i_res].mfe.tors/PH2KCAL,
                       mfe_res[i_res].mfe.vdw0/PH2KCAL, 
                       mfe_res[i_res].mfe.vdw1/PH2KCAL,
                       mfe_res[i_res].mfe.vpws/PH2KCAL,
                       mfe_res[i_res].mfe.dsol/PH2KCAL, 
                       mfe_res[i_res].mfe.ebkb/PH2KCAL,
                       mfe_res[i_res].mfe.epws/PH2KCAL,
                       mfe_res[i_res].mfe.total/PH2KCAL);
            }

        for (j=0; j<n_all; j++) {
            head1[0]='\0'; head2[0]='\0';
            if (fabs(mfe_res[i_res].mfe.mfePair[j]) < cut_off) continue;
            strcpy(head1, shead[i_res]);
            strncpy(head2, conflist.conf[all_res[j].conf[0]].uniqID, 3); head2[3] = '\0';
            strncat(head2, conflist.conf[all_res[j].conf[0]].uniqID+5, 6); head2[9] = '\0';
            if (env.titr_type == 'p') 
                fprintf(res_fp, "%s  %s   %8.2f%8.2f%8.2f%8.2f\n", head1, head2, 
                       mfe_res[i_res].mfe.vdw[j]/PH2KCAL, mfe_res[i_res].mfe.ele[j]/PH2KCAL, mfe_res[i_res].mfe.mfePair[j]/PH2KCAL, 
                       (all_res[j].mfe.crg[i_low]*(1-ratio) + all_res[j].mfe.crg[i_high]*ratio));
            else
                fprintf(res_fp, "%s  %s   %8.1f%8.1f%8.1f%8.2f\n", head1, head2, 
                       mfe_res[i_res].mfe.vdw[j]/mev2Kcal, mfe_res[i_res].mfe.ele[j]/mev2Kcal, mfe_res[i_res].mfe.mfePair[j]/mev2Kcal, 
                       (all_res[j].mfe.crg[i_low]*(1-ratio) + all_res[j].mfe.crg[i_high]*ratio));
        }
     }

     return 0;
}
          
int postrun_get_mfe(int i_res, int t_point)
{
    // get the mfe of the ith mfe_res at titration point t_point
    int j, k, q;
    float ph, eh;
    double *nocc, *rocc, socc[2]={0.0, 0.0};
    double Eref[2];
    MFE E_ground, E_ionize;
    float *mfe, *epws, *vpws;

    memset(&E_ground, 0, sizeof(MFE));
    memset(&E_ionize, 0, sizeof(MFE));
    memset(&socc, 0, 2*sizeof(float));
    E_ground.mfePair = (float *) calloc(n_all, sizeof(float));
    E_ground.vdw = (float *) calloc(n_all, sizeof(float));
    E_ground.ele = (float *) calloc(n_all, sizeof(float));
    E_ionize.mfePair = (float *) calloc(n_all, sizeof(float));
    E_ionize.vdw = (float *) calloc(n_all, sizeof(float));
    E_ionize.ele = (float *) calloc(n_all, sizeof(float));
    mfe = (float *) calloc(mfe_res[i_res].n, sizeof(float));
    epws = (float *) calloc(mfe_res[i_res].n, sizeof(float));
    vpws = (float *) calloc(mfe_res[i_res].n, sizeof(float));
    nocc = (double *) calloc(mfe_res[i_res].n, sizeof(double));
    rocc = (double *) calloc(mfe_res[i_res].n, sizeof(double));

    ph = env.titr_ph0;
    eh = env.titr_eh0;
    if (env.titr_type == 'p') ph = env.titr_ph0 + t_point*env.titr_phd;
    else eh = env.titr_eh0 + t_point*env.titr_ehd;

    for (j=0; j<mfe_res[i_res].n; j++) {
        if (!(strncmp(conflist.conf[mfe_res[i_res].conf[j]].uniqID+3, "DM", 2))) {  // dummy conformer
            conflist.conf[mfe_res[i_res].conf[j]].E_self = conflist.conf[mfe_res[i_res].conf[j]].E_self0;
            continue;
        }

        conflist.conf[mfe_res[i_res].conf[j]].E_ph =  conflist.conf[mfe_res[i_res].conf[j]].H * (ph-conflist.conf[mfe_res[i_res].conf[j]].pKa) * PH2KCAL;
        conflist.conf[mfe_res[i_res].conf[j]].E_eh =  conflist.conf[mfe_res[i_res].conf[j]].e * (eh-conflist.conf[mfe_res[i_res].conf[j]].Em) * mev2Kcal;

        for (k=0; k<conflist.n_conf; k++) {
                mfe[j] += pairwise[mfe_res[i_res].conf[j]][conflist.conf[k].iConf] * postrun_fround3(occ_table[k][t_point]);
                epws[j] += pairwise_ele[mfe_res[i_res].conf[j]][conflist.conf[k].iConf] * postrun_fround3(occ_table[k][t_point]);
                vpws[j] += pairwise_vdw[mfe_res[i_res].conf[j]][conflist.conf[k].iConf] * postrun_fround3(occ_table[k][t_point]);
        }

        conflist.conf[mfe_res[i_res].conf[j]].E_self = conflist.conf[mfe_res[i_res].conf[j]].E_self0
                                                     + conflist.conf[mfe_res[i_res].conf[j]].E_ph
                                                     + conflist.conf[mfe_res[i_res].conf[j]].E_eh
                                                     + mfe[j];
     }

    /* initial Eref */
    // '0' and 'DM' go to neutral form
    for (j=0; j<mfe_res[i_res].n; j++) {
        if (conflist.conf[mfe_res[i_res].conf[j]].uniqID[3] == '0' || conflist.conf[mfe_res[i_res].conf[j]].uniqID[3] == 'D') {
            Eref[0] = conflist.conf[mfe_res[i_res].conf[j]].E_self;
            break;
        }
    }
    for (j=0; j<mfe_res[i_res].n; j++) {
        if (conflist.conf[mfe_res[i_res].conf[j]].uniqID[3] != '0' && conflist.conf[mfe_res[i_res].conf[j]].uniqID[3] != 'D') {
            Eref[1] = conflist.conf[mfe_res[i_res].conf[j]].E_self;
            break;
        }
    }
    
      /* get Eref for ground and ionized state */
      /* if there is only one kind of conformer, this may be a problem */
    for (j=0; j<mfe_res[i_res].n; j++) {
        if (conflist.conf[mfe_res[i_res].conf[j]].uniqID[3] == '0' || conflist.conf[mfe_res[i_res].conf[j]].uniqID[3] == 'D') {
            if (Eref[0] > conflist.conf[mfe_res[i_res].conf[j]].E_self) Eref[0] = conflist.conf[mfe_res[i_res].conf[j]].E_self;
        }
        else {
            if (Eref[1] > conflist.conf[mfe_res[i_res].conf[j]].E_self) Eref[1] = conflist.conf[mfe_res[i_res].conf[j]].E_self;
        }
    }

    for (j=0; j<mfe_res[i_res].n; j++) {
        if (conflist.conf[mfe_res[i_res].conf[j]].uniqID[3] == '0' || conflist.conf[mfe_res[i_res].conf[j]].uniqID[3] == 'D') {
            rocc[j] = exp(-(conflist.conf[mfe_res[i_res].conf[j]].E_self-Eref[0]) * KCAL2KT);
        }
        else {
            rocc[j] = exp(-(conflist.conf[mfe_res[i_res].conf[j]].E_self-Eref[1]) * KCAL2KT);
        }
    }

    for (j=0; j<mfe_res[i_res].n; j++) {
        if (conflist.conf[mfe_res[i_res].conf[j]].uniqID[3] == '0' || conflist.conf[mfe_res[i_res].conf[j]].uniqID[3] == 'D') socc[0] += rocc[j];
        else socc[1] += rocc[j];
    }

    for (j=0; j<mfe_res[i_res].n; j++) {
        if (conflist.conf[mfe_res[i_res].conf[j]].uniqID[3] == '0' || conflist.conf[mfe_res[i_res].conf[j]].uniqID[3] == 'D') {
            nocc[j] = rocc[j]/socc[0];
        }
        else {
            nocc[j] = rocc[j]/socc[1];
        }
    }


    for (j=0; j<mfe_res[i_res].n; j++) {
        if (strchr(conflist.conf[mfe_res[i_res].conf[j]].uniqID, '+') || strchr(conflist.conf[mfe_res[i_res].conf[j]].uniqID, '-')) {
            E_ionize.vdw0 += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_vdw0; 
            E_ionize.vdw1 += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_vdw1; 
            E_ionize.ebkb += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_epol; 
            E_ionize.tors += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_tors; 
            E_ionize.dsol += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_dsolv; 
            E_ionize.offset += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_extra; 
            E_ionize.pHpK0 += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_ph; 
            E_ionize.EhEm0 += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_eh; 
            if (nocc[j] > 0.000001) E_ionize.TS += nocc[j] * log(nocc[j])/KCAL2KT;
            E_ionize.residues += nocc[j] * mfe[j];
            E_ionize.epws += nocc[j] * epws[j];
            E_ionize.vpws += nocc[j] * vpws[j];
            for (k=0; k<n_all; k++) {
                for (q=0; q<all_res[k].n; q++) {
                    E_ionize.mfePair[k] += nocc[j] * (pairwise[mfe_res[i_res].conf[j]][all_res[k].conf[q]] * postrun_fround3(occ_table[all_res[k].conf[q]][t_point])) ;
                    E_ionize.vdw[k] += nocc[j] * (pairwise_vdw[mfe_res[i_res].conf[j]][all_res[k].conf[q]] * postrun_fround3(occ_table[all_res[k].conf[q]][t_point])) ;
                    E_ionize.ele[k] += nocc[j] * (pairwise_ele[mfe_res[i_res].conf[j]][all_res[k].conf[q]] * postrun_fround3(occ_table[all_res[k].conf[q]][t_point])) ;
                }
                //E_ionize.vpws += E_ionize.vdw[k];
                //E_ionize.epws += E_ionize.ele[k];
            }
            if (env.mfe_pka){
                E_ionize.total = E_ionize.vdw0 + E_ionize.vdw1 + E_ionize.ebkb + E_ionize.tors + E_ionize.dsol + E_ionize.offset + E_ionize.pHpK0 + E_ionize.EhEm0 + E_ionize.TS + E_ionize.residues;
            }
            else {
                E_ionize.total = E_ionize.offset + E_ionize.tors + E_ionize.vdw0 + E_ionize.vdw1 + E_ionize.vpws + E_ionize.dsol + E_ionize.ebkb + E_ionize.epws;
            }
        }
        else {
            E_ground.vdw0 += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_vdw0;                      
            E_ground.vdw1 += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_vdw1;
            E_ground.ebkb += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_epol;
            E_ground.tors += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_tors;
            E_ground.dsol += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_dsolv;
            E_ground.offset += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_extra;
            E_ground.pHpK0 += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_ph;
            E_ground.EhEm0 += nocc[j] * conflist.conf[mfe_res[i_res].conf[j]].E_eh;
            if (nocc[j] > 0.000001) E_ground.TS += nocc[j] * log(nocc[j])/KCAL2KT;
            E_ground.residues += nocc[j] * mfe[j];
            E_ground.epws += nocc[j] * epws[j];
            E_ground.vpws += nocc[j] * vpws[j];
            for (k=0; k<n_all; k++) {
                for (q=0; q<all_res[k].n; q++) {
                   E_ground.mfePair[k] += nocc[j] * pairwise[mfe_res[i_res].conf[j]][all_res[k].conf[q]] * postrun_fround3(occ_table[all_res[k].conf[q]][t_point]);
                   E_ground.vdw[k] += nocc[j] * (pairwise_vdw[mfe_res[i_res].conf[j]][all_res[k].conf[q]] * postrun_fround3(occ_table[all_res[k].conf[q]][t_point])) ;
                   E_ground.ele[k] += nocc[j] * (pairwise_ele[mfe_res[i_res].conf[j]][all_res[k].conf[q]] * postrun_fround3(occ_table[all_res[k].conf[q]][t_point])) ;
                }
                //E_ground.vpws += E_ground.vdw[k];
                //E_ground.epws += E_ground.ele[k];
            }
            if (env.mfe_pka){
                E_ground.total = E_ground.vdw0 + E_ground.vdw1 + E_ground.ebkb + E_ground.tors + E_ground.dsol + E_ground.offset + E_ground.pHpK0 + E_ground.EhEm0 + E_ground.TS + E_ground.residues;
            }else {
                E_ground.total = E_ground.offset + E_ground.tors + E_ground.vdw0 + E_ground.vdw1 + E_ground.vpws + E_ground.dsol + E_ground.ebkb + E_ground.epws;
            }
        } 
    }

    mfe_res[i_res].mfe.vdw0 = E_ionize.vdw0 - E_ground.vdw0;
    mfe_res[i_res].mfe.vdw1 = E_ionize.vdw1 - E_ground.vdw1;
    mfe_res[i_res].mfe.ebkb = E_ionize.ebkb - E_ground.ebkb;
    mfe_res[i_res].mfe.tors = E_ionize.tors - E_ground.tors;
    mfe_res[i_res].mfe.dsol = E_ionize.dsol - E_ground.dsol;
    mfe_res[i_res].mfe.offset = E_ionize.offset - E_ground.offset;
    mfe_res[i_res].mfe.pHpK0 = E_ionize.pHpK0 - E_ground.pHpK0;
    mfe_res[i_res].mfe.EhEm0 = E_ionize.EhEm0 - E_ground.EhEm0;
    mfe_res[i_res].mfe.TS = E_ionize.TS - E_ground.TS;
    mfe_res[i_res].mfe.residues = E_ionize.residues - E_ground.residues;
    mfe_res[i_res].mfe.total = E_ionize.total - E_ground.total;
    for (j=0; j<n_all; j++) {
        mfe_res[i_res].mfe.mfePair[j] = E_ionize.mfePair[j] - E_ground.mfePair[j];
        mfe_res[i_res].mfe.vdw[j] = E_ionize.vdw[j] - E_ground.vdw[j];
        mfe_res[i_res].mfe.ele[j] = E_ionize.ele[j] - E_ground.ele[j];
    }
    mfe_res[i_res].mfe.vpws = E_ionize.vpws - E_ground.vpws;
    mfe_res[i_res].mfe.epws = E_ionize.epws - E_ground.epws;

    free(E_ground.mfePair); free(E_ground.vdw); free(E_ground.ele);
    free(E_ionize.mfePair); free(E_ionize.vdw); free(E_ionize.ele);
    free(mfe); free(epws); free(vpws); free(nocc); free(rocc);

    return 0;
}

struct STAT postrun_fit(float a, float b)
/* initialize the simplex and set up optimization */
{  float **p;
    float y[3];
    int neval;
    struct STAT result;
    
    p = (float **) malloc(3 * sizeof(float *));
    p[0] = (float *) malloc(2 * sizeof(float));
    p[1] = (float *) malloc(2 * sizeof(float));
    p[2] = (float *) malloc(2 * sizeof(float));
    
    p[0][0] = a;      p[0][1] = b;      y[0] = postrun_score(p[0]);
    p[1][0] = a+0.01; p[1][1] = b;      y[1] = postrun_score(p[1]);
    p[2][0] = a;      p[2][1] = b+1.0;  y[2] = postrun_score(p[2]);
    
    postrun_dhill(p, y, 2, 0.0001, &postrun_score, &neval);
    
    /* DEBUG
    printf("(%8.3f%8.3f)=%8.3f exit at %d\n", p[0][0], p[0][1], y[0], neval);
    */
    
    /* postrun_fit this eq. y = exp(a(x-b))/(1+exp(a(x-b))) */
    
    result.a = p[0][0];
    result.b = p[0][1];
    result.chi2 = y[0];
    
    free(p[0]);
    free(p[1]);
    free(p[2]);
    free(p);
    
    return result;
}


float postrun_score(float v[])
{  float S = 0.0;
    float yt;
    float T;
    int i;
    
    for (i=0; i<Nx; i++) {
        T = exp(v[0]*(xp[i]-v[1]));
        yt = T/(1.0+T);
        S += (yt-yp[i])*(yt-yp[i]);
    }
    
    return S;
}



                 
void postrun_dhill(float **p, float *y, int ndim, float ftol, float (*funk)(float []), int *nfunk)
{
    float postrun_dhtry(float **p, float *y, float *psum, int ndim, float (*funk)(float []), int ihi, float fac);
    int i, ihi, ilo, inhi,j, mpts = ndim+1;
    float rtol,sum,swap,ysave,ytry,*psum;
    
    
    psum = (float *)malloc(ndim * sizeof(float));
    *nfunk = 0;
    GET_PSUM
    
    for (;;) {
        ilo = 0;
        ihi = y[0] > y[1] ? (inhi = 1,0):(inhi = 0,1);
        for(i=0; i<mpts;i++) {
            if(y[i] <= y[ilo]) ilo=i;
            if(y[i] > y[ihi]) {
                inhi = ihi;
                ihi  = i;
            }
            else if (y[i] > y[inhi] && i != ihi) inhi = i;
        }
        
        /* DEBUG
        for (i=0; i<mpts; i++) {
            printf("%8.3f at (%8.3f, %8.3f)\n", y[i], p[i][0], p[i][1]);
        }
        printf("\n");
        */
        
        rtol = 2.0*fabs(y[ihi]-y[ilo])/(fabs(y[ihi])+fabs(y[ilo])+TINY);
        
        if(rtol < ftol || *nfunk >= NMAX) {
            SWAP(y[0],y[ilo])
            for (i=0; i<ndim; i++) SWAP(p[0][i],p[ilo][i])
                break;
        }
        
        *nfunk += 2;
        
        ytry = postrun_dhtry(p,y,psum,ndim,funk,ihi,-1.0);
        
        if (ytry <= y[ilo]) ytry=postrun_dhtry(p,y,psum,ndim,funk,ihi,2.0);
        else if (ytry >= y[inhi]) {
            ysave = y[ihi];
            ytry=postrun_dhtry(p,y,psum,ndim,funk,ihi,0.5);
            if (ytry >= ysave) {
                for (i=0; i<mpts; i++) {
                    if (i !=ilo) {
                        for (j=0; j<ndim; j++)
                            p[i][j] = psum[j] = 0.5*(p[i][j]+p[ilo][j]);
                        y[i] = (*funk)(psum);
                    }
                }
                *nfunk += ndim;
                GET_PSUM
            }
        }
        else --(*nfunk);
    }
    free(psum);
}

float postrun_dhtry(float **p, float *y, float *psum, int ndim, float (*funk)(float []), int ihi, float fac)
{ 
    int j;
    float fac1, fac2, ytry, *ptry;
    
    ptry = (float *)malloc(ndim*sizeof(float));
    fac1 = (1.0-fac)/ndim;
    fac2 = fac1 - fac;
    for (j=0; j<ndim; j++) ptry[j]=psum[j]*fac1-p[ihi][j]*fac2;
    ytry = (*funk)(ptry);   /* evaluate the function at the trial point */
    if (ytry < y[ihi]) {    /* if it's better than the highest, then replace the highest */
        y[ihi] = ytry;
        for (j=0; j<ndim; j++) {
            psum[j] += ptry[j] - p[ihi][j];
            p[ihi][j] = ptry[j];
        }
    }
    free(ptry);
    return ytry;
}


int postrun_load_occupancy() 
{
   FILE *fp;
   int conf_counter, titr_step_counter, j;
   char *tok;
   char sbuff[MAXCHAR_LINE];
   
   fp = fopen(OCC_TABLE, "r");
   
   /* skip 1 lines */
   for (j=0; j<1; j++) fgets(sbuff, sizeof(sbuff), fp);

   conf_counter = 0;
   while (fgets(sbuff, sizeof(sbuff), fp)) {
      
      tok = strtok (sbuff," \0");
      titr_step_counter = 0;
      while (tok != NULL && titr_step_counter < env.titr_steps)
      {
         tok = strtok (NULL, " \0");
         //printf ("%f\n",atof(tok));
         //fflush(stdout);
         occ_table[conf_counter][titr_step_counter] = atof(tok);
         ++titr_step_counter;
      }
      ++conf_counter;
   }
   
   fclose(fp);
   return 0;
}







