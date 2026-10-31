/*
 * qst_main.c - entry point of the helper self-test (see qselftest.c).
 * PPC: runs the copies built by both Xenon compilers.  -DQST_HOST: the
 * host-compiled copy, whose output is the reference.
 */
#include "qrt.h"

int qrt_main(int argc, char **argv);

#ifdef QST_HOST
void host_qselftest(void);

int qrt_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	host_qselftest();
	qrt_flush();
	return 0;
}
#else
void xenon3_qselftest(void);
void old16_qselftest(void);

int qrt_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	qrt_install_crash_handlers();
	qrt_set_context("qselftest", "xenon3");
	xenon3_qselftest();
	qrt_set_context("qselftest", "old16");
	old16_qselftest();
	qrt_flush();
	return 0;
}
#endif
