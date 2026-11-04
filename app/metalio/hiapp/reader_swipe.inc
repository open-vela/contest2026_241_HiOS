// Opt-in reading surface only. Buttons, lists and other applications keep
// their existing gestures. The worker renders the next model asynchronously.
struct ReaderSwipe {
 lv_point_t start{};uint32_t since=0;bool armed=false,pending=false;
 void*context=nullptr;bool(*turn)(void*,bool)=nullptr;
};
void ReaderSwipeEvent(lv_event_t*e){
 auto*g=(ReaderSwipe*)lv_event_get_user_data(e);auto*i=lv_indev_active();
 if(!i||lv_indev_get_type(i)!=LV_INDEV_TYPE_POINTER)return;
 auto code=lv_event_get_code(e);lv_point_t p;lv_indev_get_point(i,&p);
 if(code==LV_EVENT_PRESSED){g->start=p;g->since=lv_tick_get();g->armed=!g->pending;}
 else if(code==LV_EVENT_PRESS_LOST)g->armed=false;
 else if(code==LV_EVENT_RELEASED&&g->armed){
  g->armed=false;int dx=p.x-g->start.x,dy=p.y-g->start.y;
  if(lv_tick_elaps(g->since)<=1800&&abs(dx)>=70&&abs(dx)>2*abs(dy)&&g->turn)
   g->pending=g->turn(g->context,dx<0);
 }
}
void BindReaderSwipe(lv_obj_t*o,ReaderSwipe*g){
 lv_obj_add_event_cb(o,ReaderSwipeEvent,LV_EVENT_ALL,g);
 for(unsigned i=0;i<lv_obj_get_child_count(o);++i)BindReaderSwipe(lv_obj_get_child(o,i),g);
}
