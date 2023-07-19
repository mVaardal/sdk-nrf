#include "audio_lc3.h"

#define AUDIO_LC3_STACK_SIZE 1024

static struct k_thread audio_lc3_thread_data;
static k_tid_t audio_lc3_thread_id;

K_THREAD_STACK_DEFINE(audio_lc3_thread_stack, AUDIO_LC3_STACK_SIZE);

static void audio_lc3_play(){
	while(1){
		printk("Javel ja\n");
		k_msleep(1000);
	}
}

void audio_lc3_init(){
	int ret;

	audio_lc3_thread_id =
		k_thread_create(&audio_lc3_thread_data, audio_lc3_thread_stack,
				AUDIO_LC3_STACK_SIZE, (k_thread_entry_t)audio_lc3_play,
				NULL, NULL, NULL,
				K_PRIO_PREEMPT(CONFIG_ENCODER_THREAD_PRIO), 0, K_NO_WAIT);
	ret = k_thread_name_set(audio_lc3_thread_id, "ENCODER");
	if (ret < 0){
		printk("nEIIIIIII!!!\n");
	}
}