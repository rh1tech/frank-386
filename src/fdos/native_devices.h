#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void ConIntr(dos_far_ptr rq);
void PrnIntr(dos_far_ptr rq);
void AuxIntr(dos_far_ptr rq);
void Lpt1Intr(dos_far_ptr rq);
void Lpt2Intr(dos_far_ptr rq);
void Lpt3Intr(dos_far_ptr rq);
void Com2Intr(dos_far_ptr rq);
void Com3Intr(dos_far_ptr rq);
void Com4Intr(dos_far_ptr rq);
void ClkEntry(dos_far_ptr rq);
void BlkEntry(dos_far_ptr rq);
void NulIntr(dos_far_ptr rq);

/* Return non-zero when dhp names an ATTR_NATIVE device and the request was
 * dispatched.  status receives the request's resulting r_status. */
int fdos_native_execrh(dos_far_ptr dhp, dos_far_ptr rq, UWORD *status);

/* Read only the two x86 entry offsets required by x86_execrh(). */
void fdos_x86_dhdr_entries(dos_far_ptr dhp, UWORD *strategy, UWORD *interrupt);

#ifdef __cplusplus
}
#endif
