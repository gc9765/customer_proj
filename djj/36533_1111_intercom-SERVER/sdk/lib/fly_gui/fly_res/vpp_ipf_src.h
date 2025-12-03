#ifndef _VPP_IPF_SRC_H_
#define _VPP_IPF_SRC_H_

#define RAHMEN_MAX_NUMS 4


typedef struct {
    uint32_t data_size;     /**< Size of the image in bytes*/
    const uint8_t * data;   /**< Pointer to the data of the image*/
} _ipf_img_src_;

extern uint8_t *vpp_ifp_addr;
extern uint8_t rahmen_index;
extern const _ipf_img_src_ *ipf_imgSrcTable[];
void get_ifp_addr(_ipf_img_src_ * img_src);


#endif
