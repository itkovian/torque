#include "license_pbs.h" /* See here for the software license */
#include "pbsd_init.h"
#include "test_pbsd_init.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <log.h>
#include "pbs_error.h"

int mk_subdirs(char **);

extern char global_log_ext_msg[LOCAL_LOG_BUF_SIZE];

START_TEST(test_mk_subdirs)
  {
  char *paths[3] = { NULL };

  // null path - expect failure
  fail_unless(mk_subdirs(NULL) != PBSE_NONE);
  fail_unless(global_log_ext_msg[0] == '\0');

  // no paths defined - expect success
  fail_unless(mk_subdirs(paths) == PBSE_NONE);
  fail_unless(global_log_ext_msg[0] == '\0');

  // path defined - expect success
  fail_unless((paths[0] = strdup("./subdir")) != NULL);
  fail_unless(mk_subdirs(paths) == PBSE_NONE);
  fail_unless(strncmp("created", global_log_ext_msg, strlen("created")) == 0);

  // path defined - expect success (no log msg since already created)
  global_log_ext_msg[0] = '\0';
  fail_unless((paths[0] = strdup("./subdir")) != NULL);
  fail_unless(mk_subdirs(paths) == PBSE_NONE);
  fail_unless(global_log_ext_msg[0] == '\0');

  // clean up
  fail_unless(system("rmdir ./subdir*") == 0);
  }
END_TEST


START_TEST(test_two)
  {

  }
END_TEST

Suite *pbsd_init_suite(void)
  {
  Suite *s = suite_create("pbsd_init_suite methods");
  TCase *tc_core = tcase_create("test_mk_subdirs");
  tcase_add_test(tc_core, test_mk_subdirs);
  suite_add_tcase(s, tc_core);

  tc_core = tcase_create("test_two");
  tcase_add_test(tc_core, test_two);
  suite_add_tcase(s, tc_core);

  return s;
  }

void rundebug()
  {
  }

int main(void)
  {
  int number_failed = 0;
  SRunner *sr = NULL;
  rundebug();
  sr = srunner_create(pbsd_init_suite());
  srunner_set_log(sr, "pbsd_init_suite.log");
  srunner_run_all(sr, CK_NORMAL);
  number_failed = srunner_ntests_failed(sr);
  srunner_free(sr);
  return number_failed;
  }
