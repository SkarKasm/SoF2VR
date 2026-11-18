/* Fit only the container; held and stowed guns retain authored scale. */
#ifndef SOF2_HOLSTER_FITS_H
#define SOF2_HOLSTER_FITS_H
typedef struct {int id;float side,front,scale[3];} VRHolsterFit;
static const VRHolsterFit vrHolsterFits[]={
{2,1.34672391f,0.73270226f,{1.58354008f,1.22114883f,1.45065230f}},
{3,0.67203204f,0.72824240f,{1.78355292f,1.43487513f,1.51586339f}},
{4,1.19996502f,1.00225437f,{1.80889762f,1.68163964f,1.64394783f}},
};
#endif
