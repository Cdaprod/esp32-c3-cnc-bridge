#include <assert.h>
#include <string.h>
#include "mpg_grbl_status.h"
int main(void) {
 mpg_grbl_status_t s={0};
 assert(!mpg_grbl_status_fresh_idle(&s,100,200));
 const char *line="<Idle|MPos:1.000,2.000,3.000|FS:0,0>";
 assert(mpg_grbl_status_parse(&s,line,strlen(line),100));
 assert(mpg_grbl_status_fresh_idle(&s,250,200));
 assert(!mpg_grbl_status_fresh_idle(&s,301,200));
 assert(!mpg_grbl_status_parse(&s,"<Other|X:1>",11,400));
 assert(!mpg_grbl_status_fresh_idle(&s,401,200));
 const char *alarm="<Alarm|MPos:0,0,0>";
 assert(mpg_grbl_status_parse(&s,alarm,strlen(alarm),500));
 assert(s.alarm && !s.idle);
 assert(!mpg_grbl_status_fresh_idle(&s,500,200));

 /* GRBL v1.1 appends documented numeric substates to Hold and Door. */
 const char *substates[]={
  "<Hold:0|MPos:0,0,0>", "<Hold:1|MPos:0,0,0>",
  "<Door:0|MPos:0,0,0>", "<Door:1|MPos:0,0,0>",
  "<Door:2|MPos:0,0,0>", "<Door:3|MPos:0,0,0>"
 };
 for (unsigned i=0;i<sizeof(substates)/sizeof(substates[0]);i++) {
  assert(mpg_grbl_status_parse(&s,substates[i],strlen(substates[i]),600+i));
  assert(!s.idle && !mpg_grbl_status_fresh_idle(&s,600+i,200));
  assert(strcmp(s.line,substates[i])==0);
 }
 const uint32_t last_valid=s.updated_ms;
 assert(!mpg_grbl_status_parse(&s,"<Hold:2|MPos:0,0,0>",
                              strlen("<Hold:2|MPos:0,0,0>"),700));
 assert(!mpg_grbl_status_parse(&s,"<Door:4|MPos:0,0,0>",
                              strlen("<Door:4|MPos:0,0,0>"),700));
 assert(!mpg_grbl_status_parse(&s,"<Hold:10|MPos:0,0,0>",
                              strlen("<Hold:10|MPos:0,0,0>"),700));
 assert(!mpg_grbl_status_parse(&s,"<Door:x|MPos:0,0,0>",
                              strlen("<Door:x|MPos:0,0,0>"),700));
 assert(s.updated_ms==last_valid);
 return 0;
}
