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
 return 0;
}
