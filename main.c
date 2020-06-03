#include <math.h>

#define LPS_IMPLEMENTATION
#include "lps.h"

static void
sub_cb(void *opaque, const char *topic, lps_kind_t kind, ...)
{
  printf("%s = ", topic);

  va_list ap;
  va_start(ap, kind);
  switch(kind) {
  case LPS_VALUE:
    printf("(value) %f\n", va_arg(ap, double));
    break;
  case LPS_COUNTER:
    printf("(counter) %f\n", va_arg(ap, double));
    break;
  case LPS_REQUEST_VALUE:
    printf("(request) %f\n", va_arg(ap, double));
    break;
  }
  va_end(ap);
}



int
main(int argc, char **argv)
{
  int c;
  const char *domain = "lps";
  const char *topic = NULL;
  double value = NAN;
  double requested_value = NAN;

  while((c = getopt(argc, argv, "d:t:v:r:")) != -1) {
    switch(c) {
    case 'd':
      domain = optarg;
      break;
    case 't':
      topic = optarg;
      break;
    case 'v':
      value = strtod(optarg, NULL);
      break;
    case 'r':
      requested_value = strtod(optarg, NULL);
      break;
    }
  }


  if(topic == NULL) {
    fprintf(stderr, "No topic?\n");
    exit(1);
  }
  lps_t *lps = lps_start(domain);
  if(lps == NULL) {
    exit(1);
  }

  lps_sub(lps, topic, sub_cb, NULL);

  if(!isnan(value)) {
    lps_pub(lps, topic, LPS_VALUE, value);
  }
  if(!isnan(requested_value)) {
    printf("wat\n");
    lps_pub(lps, topic, LPS_REQUEST_VALUE, requested_value);
  }
  pause();
}
