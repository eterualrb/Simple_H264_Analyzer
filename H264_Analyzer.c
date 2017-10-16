#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * NAL Unit 类型
 */
typedef enum {
	NALU_TYPE_SLICE    = 1,
	NALU_TYPE_DPA      = 2,
	NALU_TYPE_DPB      = 3,
	NALU_TYPE_DPC      = 4,
	NALU_TYPE_IDR      = 5,
	NALU_TYPE_SEI      = 6,
	NALU_TYPE_SPS      = 7,
	NALU_TYPE_PPS      = 8,
	NALU_TYPE_AUD      = 9,
	NALU_TYPE_EOSEQ    = 10,
	NALU_TYPE_EOSTREAM = 11,
	NALU_TYPE_FILL     = 12,
} NaluType;

/**
 * NAL Unit 优先级
 */
typedef enum {
	NALU_PRIORITY_DISPOSABLE = 0,
	NALU_PRIRITY_LOW         = 1,
	NALU_PRIORITY_HIGH       = 2,
	NALU_PRIORITY_HIGHEST    = 3
} NaluPriority;

/**
 * NAL Unit 结构体
 */
typedef struct {
	int startcodeprefix_len;      // 4 for parameter sets and first slice in picture, 3 for everything else (suggested)
	unsigned len;                 // Length of the NAL unit (Excluding the start code, which does not belong to the NALU)
	unsigned max_size;            // Nal Unit Buffer size
	int forbidden_bit;            // should be always FALSE
	int nal_reference_idc;        // NALU_PRIORITY_xxxx
	int nal_unit_type;            // NALU_TYPE_xxxx    
	char *buf;                    // contains the first byte followed by the EBSP
} NALU_t;

/**
 * 测试文件handler的全局指针
 */
FILE* gH264StreamPtr = NULL;

/**
 * 标识是否查找到0x 00 00 01 prefix
 */
int info2 = 0;

/**
 * 标识是否查找到0x 00 00 00 01 prefix
 */
int info3 = 0;

/**
 * 查找AnnexB字节流格式的起始码 0x 00 00 01
 * @param  buf [description]
 * @return     [description]
 */
int findStartCode2(unsigned char* buf) {
	if(buf[0]!=0 || buf[1]!=0 || buf[2] !=1) {
		return 0;
	} else {
		return 1;	
	}
}

/**
 * 查找AnnexB字节流格式的起始码 0x 00 00 00 01
 * @param  buf [description]
 * @return     [description]
 */
int findStartCode3(unsigned char* buf) {
	if(buf[0]!=0 || buf[1]!=0 || buf[2] !=0 || buf[3] !=1) {
		return 0;
	} else {
		return 1;	
	}
}

/**
 * 提取附录B字节流格式的NAL Unit
 * 
 * @param  nalu [description]
 * @return      [description]
 */
int getAnnexbNALU(NALU_t* nalu) {
	int pos = 0;
	int StartCodeFound, rewind;
	unsigned char *tmpBuf = (unsigned char *)calloc(nalu->max_size, sizeof(char));
	if (NULL == tmpBuf) {
		printf("getAnnexbNALU : 分配NAL Unit临时缓冲区失败\n");
		return -1;
	}

	nalu->startcodeprefix_len = 3; // NAL Unit的prefix code一般为0x 00 00 01，所以默认其长度为3
	if (3 != fread(tmpBuf, 1, 3, gH264StreamPtr)) {
		free(tmpBuf);
		return -1;
	}

	info2 = findStartCode2(tmpBuf); // 判断接下来要分析的数据是否以0x 00 00 01开始
	if (1 != info2) { // 不是以0x 00 00 01开始
		if (1 != fread(tmpBuf + 3, 1, 1, gH264StreamPtr)) { // 再读取一个字节，因为可能是以0x 00 00 00 01开始
			free(tmpBuf);
			return -1;
		}
		info3 = findStartCode3(tmpBuf); // 判断是否以0x 00 00 00 01开始
		if (1 != info3) { // 数据既不是以0x 00 00 01开始，也不是以0x 00 00 00 01开始，说明分析的码流有问题
			free(tmpBuf);
			return -1;
		} else {
			nalu->startcodeprefix_len = 4;
			pos = 4;
		}
	} else {
		nalu->startcodeprefix_len = 3;
		pos = 3;
	}

	// 查找下一个prefix code的位置，两个prefix code之间的数据才是NAL Unit的负载数据
	StartCodeFound = 0; // 是否找到下一个prefix
	info2 = 0; // 下一个prefix code是否为0x 00 00 01
	info3 = 0; // 下一个prefix code是否为0x 00 00 00 01

	while (!StartCodeFound) {
		if (feof(gH264StreamPtr)) {
			nalu->len = (pos-1) - nalu->startcodeprefix_len;
			memcpy (nalu->buf, &tmpBuf[nalu->startcodeprefix_len], nalu->len);     
			nalu->forbidden_bit = nalu->buf[0] & 0x80; //1 bit
			nalu->nal_reference_idc = nalu->buf[0] & 0x60; // 2 bit
			nalu->nal_unit_type = (nalu->buf[0]) & 0x1f;// 5 bit
			free(tmpBuf);
			return pos - 1;
		}

		tmpBuf[pos++] = fgetc(gH264StreamPtr);
		info3 = findStartCode3(&tmpBuf[pos - 4]);
		if (1 != info3) {
			info2 = findStartCode2(&tmpBuf[pos - 3]);
		}

		StartCodeFound = (info2 == 1 || info3 == 1);
	}

	// Here, we have found another start code (and read length of startcode bytes more than we should
	// have.  Hence, go back in the file
	// 我们找到了下一个NAL Unit的prefix
	// 需要将文件指针回滚到上一个NAL Unit的末尾
	rewind = (info3 == 1)? -4 : -3;

	// 文件指针进行回滚
	if (0 != fseek (gH264StreamPtr, rewind, SEEK_CUR)){
		free(tmpBuf);
		printf("getAnnexbNALU: 文件指针回滚失败");
		return -1;
	}

	// Here the Start code, the complete NALU, and the next start code is in the Buf.  
	// The size of Buf is pos, pos+rewind are the number of bytes excluding the next
	// start code, and (pos+rewind)-startcodeprefix_len is the size of the NALU excluding the start code
	// 目前tmpBuf存储了当前NAL Unit的prefix code，负载数据以及下一个NAL Unit的prefix code
	// 在copy EBSP数据时不要copy首尾的prefix code
	nalu->len = (pos+rewind) - nalu->startcodeprefix_len; // NAL Unit负载数据(不包括prefix code)的长度
	memcpy(nalu->buf, &tmpBuf[nalu->startcodeprefix_len], nalu->len); // 拷贝NAL Unit tmp buffer中的负载数据
	nalu->forbidden_bit = nalu->buf[0] & 0x80; //1 bit
	nalu->nal_reference_idc = nalu->buf[0] & 0x60; // 2 bit
	nalu->nal_unit_type = (nalu->buf[0]) & 0x1f;// 5 bit
	free(tmpBuf);

	return (pos+rewind);
}

/**
 * 释放文件资源
 */
void releaseFileResource() {
	if (NULL != gH264StreamPtr) {
		fclose(gH264StreamPtr);
		gH264StreamPtr = NULL;
	}
}

int main(int argc, char* argv[]) {
	gH264StreamPtr = fopen(argv[1], "rb");
	if (NULL == gH264StreamPtr) {
		printf("打开h264文件失败\n");
		return -1;
	}
	// printf("打开h264文件成功\n");

	// log输出到标准输出(屏幕)中
	FILE* outPtr = stdout;

	int bufferSize = 100000;
	NALU_t* n = (NALU_t*)calloc(1, sizeof(NALU_t));	// calloc在分配内存空间后会进行初始化，而malloc不会进行初始化
	if (NULL == n) {
		printf("NALU_t内存空间分配失败\n");
		releaseFileResource();
		return -2;
	}
	n->max_size = bufferSize;
	n->buf = (char*)calloc(n->max_size, sizeof(char));
	if (NULL == n->buf) {
		printf("EBSP内存空间分配失败\n");
		releaseFileResource();
		return -3;
	}

	int data_offset=0;
	int nal_num=0;
	printf("-----+-------- NALU Table ------+---------+\n");
	printf(" NUM |    POS  |    IDC |  TYPE |   LEN   |\n");
	printf("-----+---------+--------+-------+---------+\n");

	while(!feof(gH264StreamPtr)) {
		int data_lenth = getAnnexbNALU(n); 
		if (0 > data_lenth) {
			releaseFileResource();
			return -4;
		}

		char type_str[20] = {0};
		switch(n->nal_unit_type){
			case NALU_TYPE_SLICE:
				sprintf(type_str,"SLICE");
				break;
			case NALU_TYPE_DPA:
				sprintf(type_str,"DPA");
				break;
			case NALU_TYPE_DPB:
				sprintf(type_str,"DPB");
				break;
			case NALU_TYPE_DPC:
				sprintf(type_str,"DPC");
				break;
			case NALU_TYPE_IDR:
				sprintf(type_str,"IDR");
				break;
			case NALU_TYPE_SEI:
				sprintf(type_str,"SEI");
				break;
			case NALU_TYPE_SPS:
				sprintf(type_str,"SPS");
				break;
			case NALU_TYPE_PPS:
				sprintf(type_str,"PPS");
				break;
			case NALU_TYPE_AUD:
				sprintf(type_str,"AUD");
				break;
			case NALU_TYPE_EOSEQ:
				sprintf(type_str,"EOSEQ");
				break;
			case NALU_TYPE_EOSTREAM:
				sprintf(type_str,"EOSTREAM");
				break;
			case NALU_TYPE_FILL:
				sprintf(type_str,"FILL");
				break;
		}

		char idc_str[20]={0};
		switch(n->nal_reference_idc >> 5){
			case NALU_PRIORITY_DISPOSABLE:
				sprintf(idc_str,"DISPOS");
				break;
			case NALU_PRIRITY_LOW:
				sprintf(idc_str,"LOW");
				break;
			case NALU_PRIORITY_HIGH:
				sprintf(idc_str,"HIGH");
				break;
			case NALU_PRIORITY_HIGHEST:
				sprintf(idc_str,"HIGHEST");
				break;
		}

		fprintf(outPtr, "%5d| %8d| %7s| %6s| %8d|\n", nal_num, data_offset, idc_str, type_str, n->len);

		data_offset=data_offset+data_lenth;

		nal_num++;

	}

	/*********资源释放*********/
	if (NULL != n) {
		if (NULL != n->buf) {
			free(n->buf);
			n->buf = NULL;
		}

		free(n);
		n = NULL;
	}

	releaseFileResource();
	
	return 0;
}
