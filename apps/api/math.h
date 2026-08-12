#ifndef __NATIVE_DOS_MATH_H__
#define __NATIVE_DOS_MATH_H__

#ifdef __cplusplus
extern "C" {
#endif

double trunc(double x);
double floor(double x);
double pow(double x, double y);
double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double log(double x);
double exp(double x);
float powf(float x, float y);

#ifdef __cplusplus
}
#endif

#endif /* __NATIVE_DOS_MATH_H__ */
