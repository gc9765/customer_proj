#ifndef _APP_PLAYBACK_H_
#define _APP_PLAYBACK_H_

typedef enum 
{
    FT_JPEG=0,
    FT_AVI,
    FT_MAX
} FILETYPE;

typedef struct {
    char name[26];
    FILETYPE filetype;
} file_info;
 
typedef struct FileNode {
    char name[128];
    FILETYPE filetype;
    struct FileNode *prev;
    struct FileNode *next;
} FileNode;

void jpeg_file_get(uint8* photo_name,uint32 reset, char* file);
void jpeg_photo_explain(uint8* photo_name, uint32 scale_w, uint32 scale_h);
void rec_playback_thread_init(uint8* rec_name);


int scan_album_files();
void album_file_preview(uint8_t* file_name , uint8_t type);

void free_album_list(void);
void print_album_list(void);
void album_next_file(void);
void album_prev_file(void);

extern file_info *ablumlist; 

extern FileNode *album_list_head ;
extern FileNode *album_list_tail ;
extern FileNode *album_cur_selected ;

#endif
