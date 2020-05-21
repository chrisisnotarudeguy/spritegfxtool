//#include "std-includes.h"
#include "libsmw.h"

#include <new>//placement new
#include <stdlib.h>//malloc, realloc, free
#include <string.h>//strcmp, memmove
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <cstdio>

#include "autoarray.h"
//#include "errors.h"
//#include "asar.h"
#include "crc32.h"
#include <cstdint>

static int sa1banks[8]={0<<20, 1<<20, -1, -1, 2<<20, 3<<20, -1, -1};


//mapper_t mapper=lorom;
const unsigned char * romdata= nullptr; // NOTE: Changed into const to prevent direct write access - use writeromdata() functions below
int romlen;
static bool header;
static FILE * thisfile;

asar_error_id openromerror;

autoarray<writtenblockdata> writtenblocks;


/* *********** Static functions first ************* */


// RPG Hacker: Uses binary search to find the insert position of our ROM write
static int findromwritepos(int snesoffset, int searchstartpos, int searchendpos)
{
	if (searchendpos == searchstartpos)
		return searchstartpos;

	int centerpos = searchstartpos + ((searchendpos - searchstartpos) / 2);

	if (writtenblocks[centerpos].snesoffset >= snesoffset)
		return findromwritepos(snesoffset, searchstartpos, centerpos);

	return findromwritepos(snesoffset, centerpos + 1, searchendpos);
}

static void addromwriteforbank(int snesoffset, int numbytes, mapper_t rommapper)
{
	int currentbank = (snesoffset & 0xFF0000);

	int insertpos = findromwritepos(snesoffset, 0, writtenblocks.count);

	if (insertpos > 0 && (writtenblocks[insertpos - 1].snesoffset & 0xFF0000) == currentbank
		&& writtenblocks[insertpos - 1].snesoffset + writtenblocks[insertpos - 1].numbytes >= snesoffset)
	{
		// Merge if we overlap with a preceding block
		int firstend = writtenblocks[insertpos - 1].snesoffset + writtenblocks[insertpos - 1].numbytes;
		int secondend = snesoffset + numbytes;

		int newend = (firstend > secondend ? firstend : secondend);

		numbytes = newend - writtenblocks[insertpos - 1].snesoffset;
		snesoffset = writtenblocks[insertpos - 1].snesoffset;

		writtenblocks.remove(insertpos - 1);
		insertpos -= 1;
	}

	while (insertpos <  writtenblocks.count && (writtenblocks[insertpos].snesoffset & 0xFF0000) == currentbank
		&& snesoffset + numbytes >= writtenblocks[insertpos].snesoffset)
	{
		// Merge if we overlap with a succeeding block
		int firstend = snesoffset + numbytes;
		int secondend = writtenblocks[insertpos].snesoffset + writtenblocks[insertpos].numbytes;

		int newend = (firstend > secondend ? firstend : secondend);

		numbytes = newend - snesoffset;

		writtenblocks.remove(insertpos);
	}

	// Insert ROM write
	writtenblockdata blockdata;
	blockdata.snesoffset = snesoffset;
	blockdata.pcoffset = snestopc(snesoffset, rommapper);
	blockdata.numbytes = numbytes;

	writtenblocks.insert(insertpos, blockdata);
}

static void addromwrite(int pcoffset, int numbytes, mapper_t rommapper)
{
	int snesaddr = pctosnes(pcoffset, rommapper);
	int bytesleft = numbytes;

	// RPG Hacker: Some kind of witchcraft which I actually hope works as intended
	// Basically, the purpose of this is to sort all ROM writes into banks for the sake of cleanness

	while (((snesaddr >> 16) & 0xFF) != (((snesaddr + bytesleft) >> 16) & 0xFF))
	{
		int bytesinbank = ((snesaddr + 0x10000) & 0xFF0000) - snesaddr;

		addromwriteforbank(snesaddr, bytesinbank, rommapper);

		pcoffset += bytesinbank;
		snesaddr = pctosnes(pcoffset, rommapper);
		bytesleft -= bytesinbank;
	}

	addromwriteforbank(snesaddr, bytesleft, rommapper);
}

static void handleprot(int loc, char * name, int len, const unsigned char * contents, const unsigned char *romdata)
{
	(void)loc;		// RPG Hacker: Silence "unused argument" warning.

	if (!strncmp(name, "PROT", 4))
	{
		strncpy(name, "NULL", 4);//to block recursion, in case someone is an idiot
		if (len%3) return;
		len/=3;
		for (int i=0;i<len;i++) {
            removerats( 
                (contents[(i*3)+0]) |  (contents[(i*3)+1]<<8 ) | (contents[(i*3)+2]<<16)
                , 0x00, romdata);
        }
    }
}

static int trypcfreespace(int start, int end, int size, int banksize, int minalign, unsigned char freespacebyte, const unsigned char * romdata)
{
	while (start+size<=end)
	{
		if (
				((start+8)&~banksize)!=((start+size-1)&~banksize&0xFFFFFF)//if the contents won't fit in this bank...
			&&
				(start&banksize&0xFFFFF8)!=(banksize&0xFFFFF8)//and the RATS tag can't fit in the bank either...
			)
		{
			start&=~banksize&0xFFFFFF;//round it down to the start of the bank,
			start|=banksize&0xFFFFF8;//then round it up to the end minus the RATS tag...
			continue;
		}
		if (minalign)
		{
			start&=~minalign&0xFFFFFF;
			start|=minalign&0xFFFFF8;
		}
		if (!strncmp((const char*)romdata+start, "STAR", 4) &&
				(romdata[start+4]^romdata[start+6])==0xFF && (romdata[start+5]^romdata[start+7])==0xFF)
		{
			start+=(romdata[start+4]|(romdata[start+5]<<8))+1+8;
			continue;
		}
		bool bad=false;
		for (int i=0;i<size;i++)
		{
			if (romdata[start+i]!=freespacebyte)
			{
				// TheBiob: fix freedata align freezing.
				if ((start & minalign) == 0x7FF8 && i < 8) i = 8;
				start+=i;
				if (!i) start++;//this could check for a rats tag instead, but somehow I think this will give better performance.
				bad=true;
				break;
			}
		}
		if (bad) continue;
		size-=8;
		if (size) size--;//rats tags eat one byte more than specified for some reason
		writeromdata_byte(start+0, 'S', romdata);
		writeromdata_byte(start+1, 'T', romdata);
		writeromdata_byte(start+2, 'A', romdata);
		writeromdata_byte(start+3, 'R', romdata);
		writeromdata_byte(start+4, (unsigned char)(size&0xFF), romdata);
		writeromdata_byte(start+5, (unsigned char)((size>>8)&0xFF), romdata);
		writeromdata_byte(start+6, (unsigned char)((size&0xFF)^0xFF), romdata);
		writeromdata_byte(start+7, (unsigned char)(((size>>8)&0xFF)^0xFF), romdata);
		return start+8;
	}
	return -1;
}

static unsigned int getchecksum(const unsigned char * romdata)
{
	unsigned int checksum=0;
	for (int i=0;i<romlen;i++) checksum+=romdata[i];//this one is correct for most cases, and I don't care about the rest.
	return checksum&0xFFFF;
}

static int getpcfreespace_lorom(int size, bool isforcode, bool autoexpand, 
                            bool respectbankborders, bool align, unsigned char freespacebyte, 
                            const unsigned char *romdata, mapper_t rommapper)
{
    if (size>0x8008 && respectbankborders)
        return -1;

rebootlorom:
    if (romlen>0x200000 && !isforcode) {
        int pos=trypcfreespace(0x200000-8, (romlen<0x400000)?romlen:0x400000, size,
                respectbankborders?0x7FFF:0xFFFFFF, align?0x7FFF:(respectbankborders || size<32768)?0:0x7FFF, freespacebyte,
                romdata);
        if (pos>=0) 
            return pos;
    }
    int pos=trypcfreespace(0x80000, (romlen<0x200000)?romlen:0x200000, size,
            respectbankborders?0x7FFF:0xFFFFFF, align?0x7FFF:(respectbankborders || size<32768)?0:0x7FFF, freespacebyte, romdata);
    if (pos>=0) 
        return pos;
    if (autoexpand) {
        if (0)
            ;
        else if (romlen==0x080000) {
            romlen=0x100000;
            writeromdata_byte(snestopc(0x00FFD7, rommapper), 0x0A, romdata);
        } else if (romlen==0x100000) {
            romlen=0x200000;
            writeromdata_byte(snestopc(0x00FFD7, rommapper), 0x0B, romdata);
        } else if (isforcode) 
            return -1; //no point creating freespace that can't be used
        else if (romlen==0x200000 || romlen==0x300000) {
            romlen=0x400000;
            writeromdata_byte(snestopc(0x00FFD7, rommapper), 0x0C, romdata);
        } else
            return -1;
        autoexpand=false;
        goto rebootlorom;
    }
    return -1;
}

static int getpcfreespace_sa1rom(int size, bool isforcode, bool autoexpand, 
                            bool respectbankborders, bool align, unsigned char freespacebyte, 
                            const unsigned char *romdata)
{
rebootsa1rom:
    int nextbank=-1;
    for (int i=0;i<8;i++)
    {
        if (i&2)
            continue;
        if (sa1banks[i]+0x100000>romlen)
        {
            if (sa1banks[i]<=romlen && sa1banks[i]+0x100000>romlen)
                nextbank=sa1banks[i];
            continue;
        }
        int pos=trypcfreespace(sa1banks[i]?sa1banks[i]:0x80000, sa1banks[i]+0x100000, size, 0x7FFF, align?0x7FFF:0, freespacebyte, romdata);
        if (pos>=0)
            return pos;
    }
    if (autoexpand && nextbank>=0)
    {
        unsigned char x7FD7[]={0, 0x0A, 0x0B, 0x0C, 0x0C, 0x0D, 0x0D, 0x0D, 0x0D};
        romlen=nextbank+0x100000;
        writeromdata_byte(0x7FD7, x7FD7[romlen>>20], romdata);
        autoexpand=false;
        goto rebootsa1rom;
    }
    return -1;
}











/* ********** Public functions *********** */

//rom->file
//rom->lenght

void init_rom(SnesRom *rom)
{
    rom->file = NULL;
    rom->filename = NULL;
    rom->data = NULL;
    rom->lenght = 0;
    rom->mapper = mapper_t::lorom;
    rom->header = false;
}

bool openrom(SnesRom *rom, const char * filename, bool confirm)
{
    int truelen;

	closerom();
	rom->file = fopen(filename, "r+b");
	if (!(rom->file)) {
		//openromerror = error_id_open_rom_failed;
		return false;
	}
    //rom->filename = filename;

	fseek(rom->file, 0, SEEK_END);
	rom->header = false;
	if (strlen(filename) > 4) {
		const char * fnameend = strchr(filename, '\0')-4;
		header = (!stricmp(fnameend, ".smc"));
	}

	rom->lenght = ftell(rom->file) - (header*512);
	if (rom->lenght < 0)
        rom->lenght = 0;

	fseek(rom->file, header*512, SEEK_SET);
	romdata = (unsigned char*) malloc(sizeof(unsigned char)*16*1024*1024);

	truelen = (int) fread(const_cast<unsigned char*>(romdata), 1u, (size_t) rom->lenght, rom->file);
	if (truelen!=rom->lenght) {
		openromerror = error_id_open_rom_failed;
		free(const_cast<unsigned char*>(romdata));
		return false;
	}

	memset(const_cast<unsigned char*>(romdata)+rom->lenght, 0x00, (size_t)(16*1024*1024-rom->lenght));
	if (confirm && 
        snestopc(0x00FFC0, rom->mapper) + 21<(int)rom->lenght && 
        strncmp((const char*)romdata + snestopc(0x00FFC0, rom->mapper), "SUPER MARIOWORLD     ", 21))
	{
		closerom(false);
		openromerror = header ? error_id_open_rom_not_smw_extension : error_id_open_rom_not_smw_header;
		return false;
	}
	return true;
}

uint32_t closerom(bool save, const unsigned char * romdata)
{
	uint32_t romCrc = 0;
	if (thisfile && save && romlen)
	{
		fseek(thisfile, header*512, SEEK_SET);
		fwrite(const_cast<unsigned char*>(romdata), 1, (size_t)romlen, thisfile);

		// do a quick re-read of the header, and include that in the crc32 calculation if necessary
		{
			uint8_t* filedata = (uint8_t*)malloc(sizeof(uint8_t) * (romlen + header * 512));
			if (header)
			{
				fseek(thisfile, 0, SEEK_SET);
				fread(filedata, sizeof(uint8_t), 512, thisfile);
			}
			memcpy(filedata + (header * 512), romdata, sizeof(uint8_t) * (size_t)romlen);
			romCrc = crc32(filedata, (unsigned int)(romlen + header * 512));
			free(filedata);
		}
	}
	if (thisfile) fclose(thisfile);
	if (romdata) free(const_cast<unsigned char*>(romdata));
	thisfile= nullptr;
	romdata= nullptr;
	romlen=0;
	return romCrc;
}



void writeromdata(int pcoffset, const void * indata, int numbytes, 
                const unsigned char * romdata, mapper_t rommapper)
{
	memcpy(const_cast<unsigned char*>(romdata) + pcoffset, indata, (size_t)numbytes);
	addromwrite(pcoffset, numbytes, rommapper);
}

void writeromdata_byte(int pcoffset, unsigned char indata, 
                    const unsigned char * romdata, mapper_t rommapper)
{
	memcpy(const_cast<unsigned char*>(romdata) + pcoffset, &indata, 1);
	addromwrite(pcoffset, 1, rommapper);
}

void writeromdata_bytes(int pcoffset, unsigned char indata, int numbytes,
                    const unsigned char * romdata, mapper_t rommapper)
{
	memset(const_cast<unsigned char*>(romdata) + pcoffset, indata, (size_t)numbytes);
	addromwrite(pcoffset, numbytes, rommapper);
}



int snestopc(int addr, mapper_t rommapper)
{
    if (addr < 0 || addr > 0xFFFFFF)
        return -1; //not 24bit
    
    switch (rommapper) {
    case mapper_t::lorom:
        // randomdude999: The low pages ($0000-$7FFF) of banks 70-7D are used
        // for SRAM, the high pages are available for ROM data though
        if ((addr & 0xFE0000) == 0x7E0000 || //wram
            (addr & 0x408000) == 0x000000 || //hardware regs, ram mirrors, other strange junk
            (addr & 0x708000) == 0x700000) //sram (low parts of banks 70-7D)
            return -1;
        addr = ((addr & 0x7F0000) >> 1 | (addr & 0x7FFF));
        return addr;
    case mapper_t::hirom:
        if ((addr & 0xFE0000) == 0x7E0000 || //wram
            (addr & 0x408000) == 0x000000) //hardware regs, ram mirrors, other strange junk
            return -1;
        return addr & 0x3FFFFF;
    case mapper_t::exlorom:
        if ((addr & 0xF00000) == 0x700000 || //wram, sram
            (addr & 0x408000) == 0x000000) //area that shouldn't be used in lorom
            return -1;
        if (addr & 0x800000)
            addr = ((addr & 0x7F0000) >> 1 | (addr & 0x7FFF));
        else
            addr = ((addr & 0x7F0000) >> 1 | (addr & 0x7FFF)) + 0x400000;
        return addr;
    case mapper_t::exhirom:
        if ((addr & 0xFE0000) == 0x7E0000 || //wram
            (addr & 0x408000) == 0x000000) //hardware regs, ram mirrors, other strange junk
            return -1;
        if ((addr & 0xC00000) != 0xC00000)
            return (addr & 0x3FFFFF) | 0x400000;
        return addr & 0x3FFFFF;
    case mapper_t::sfxrom:
        // Asar emulates GSU1, because apparently emulators don't support the extra ROM data from GSU2
        if ((addr & 0x600000) == 0x600000 || //wram, sram, open bus
            (addr & 0x408000) == 0x000000 || //hardware regs, ram mirrors, rom mirrors, other strange junk
            (addr & 0x800000) == 0x800000) //fastrom isn't valid either in superfx
            return -1;
        if (addr & 0x400000)
            return addr & 0x3FFFFF;
        else
            return (addr & 0x7F0000) >> 1 | (addr & 0x7FFF);
    case mapper_t::sa1rom:
        if ((addr & 0x408000) == 0x008000)
            return sa1banks[(addr & 0xE00000) >> 21] | ((addr & 0x1F0000) >> 1) | (addr & 0x007FFF);
        if ((addr & 0xC00000) == 0xC00000)
            return sa1banks[((addr & 0x100000) >> 20) | ((addr & 0x200000) >> 19)] | (addr & 0x0FFFFF);
        return -1;
    case mapper_t::bigsa1rom:
        if ((addr & 0xC00000) == 0xC00000) //hirom
            return (addr & 0x3FFFFF) | 0x400000;
        if ((addr & 0xC00000) == 0x000000 || (addr & 0xC00000) == 0x800000) //lorom
            if ((addr & 0x008000) == 0x000000)
                return -1;
            return (addr & 0x800000) >> 2 | (addr & 0x3F0000) >> 1 | (addr & 0x7FFF);
        return -1;
    case mapper_t::norom:
        return addr;
    }
    return -1;
}

int pctosnes(int addr, mapper_t rommapper)
{
    if (addr < 0)
        return -1;

    switch (rommapper) {
    case mapper_t::lorom:
        if (addr >= 0x400000)
            return -1;
        addr = ((addr << 1) & 0x7F0000) | (addr & 0x7FFF) | 0x8000;
        return addr | 0x800000;
    case mapper_t::hirom:
        if (addr >= 0x400000)
            return -1;
        return addr | 0xC00000;
    case mapper_t::exlorom:
        if (addr >= 0x800000)
            return -1;
        if (addr & 0x400000) {
            addr -= 0x400000;
            addr = ((addr << 1) & 0x7F0000) | (addr & 0x7FFF) | 0x8000;
            return addr;
        } else {
            addr = ((addr << 1) & 0x7F0000) | (addr & 0x7FFF) | 0x8000;
            return addr | 0x800000;
        }
    case mapper_t::exhirom:
        if (addr >= 0x800000)
            return -1;
        if (addr & 0x400000)
            return addr;
        return addr | 0xC00000;
    case mapper_t::sa1rom:
        for (int i = 0; i < 8; i++) {
            if (sa1banks[i] == (addr & 0x700000))
                return 0x008000 | (i << 21) | ((addr & 0x0F8000) << 1) | (addr & 0x7FFF);
        }
        return -1;
    case mapper_t::bigsa1rom:
        if (addr >= 0x800000)
            return -1;
        if ((addr & 0x400000) == 0x400000)
            return addr | 0xC00000;
        if ((addr & 0x600000) == 0x000000)
            return ((addr << 1) & 0x3F0000) | 0x8000 | (addr & 0x7FFF);
        if ((addr & 0x600000) == 0x200000)
            return 0x800000 | ((addr << 1) & 0x3F0000) | 0x8000 | (addr & 0x7FFF);
        return -1;
    case mapper_t::sfxrom:
        if (addr >= 0x200000)
            return -1;
        return ((addr << 1) & 0x7F0000) | (addr & 0x7FFF) | 0x8000;
    case mapper_t::norom:
        return addr;
    }
    return -1;
}




//This function finds a block of freespace. -1 means "no freespace found", anything else is a PC address.
//isforcode=false tells it to favor banks 40+, true tells it to avoid them entirely.
//It automatically adds a RATS tag.
int getpcfreespace(mapper_t rommapper, const unsigned char *romdata,
                int size, bool isforcode, bool autoexpand, 
                bool respectbankborders, bool align, unsigned char freespacebyte)
{
	if (!size)
        return 0x1234;//in case someone protects zero bytes for some dumb reason.
		//You can write zero bytes to anywhere, so I'll just return something that removerats will ignore.
	if (size>0x10000)
        return -1;
	size+=8;

    switch(rommapper) {
    case mapper_t::lorom:
        return getpcfreespace_lorom(size, isforcode, autoexpand, respectbankborders, align, freespacebyte, romdata, rommapper);
    case mapper_t::hirom:
		if (isforcode)
            return -1;
		return trypcfreespace(0, romlen, size, 0xFFFF, align?0xFFFF:0, freespacebyte, romdata);
    case mapper_t::exlorom:
		if (isforcode)
            return -1;
		return trypcfreespace(0, romlen, size, 0x7FFF, align ? 0x7FFF : 0, freespacebyte, romdata);
    case mapper_t::exhirom:
		if (isforcode)
            return -1;
		return trypcfreespace(0, romlen, size, 0xFFFF, align?0xFFFF:0, freespacebyte, romdata);
    case mapper_t::sfxrom:
		if (!isforcode)
            return -1;
		// try not to overwrite smw stuff
		return trypcfreespace(0x80000, romlen, size, 0x7FFF, align?0x7FFF:0, freespacebyte, romdata);
    case mapper_t::sa1rom:
        return getpcfreespace_sa1rom(size, isforcode, autoexpand, respectbankborders, align, freespacebyte, romdata);
    case mapper_t::bigsa1rom:
		if(!isforcode && romlen > 0x400000)
		{
			int pos=trypcfreespace(0x400000, romlen, size, 0xFFFF, align?0xFFFF:0, freespacebyte, romdata);
			if (pos>=0) 
                return pos;
		}
		int pos=trypcfreespace(0x080000, romlen, size, 0x7FFF, align?0x7FFF:0, freespacebyte, romdata);
		if (pos>=0) 
            return pos;
    }
	return -1;
}

int getsnesfreespace(mapper_t rommapper, const unsigned char *romdata,
                    int size, bool isforcode, bool autoexpand, bool respectbankborders, bool align, unsigned char freespacebyte)
{
	return pctosnes(getpcfreespace(size, isforcode, autoexpand, respectbankborders, align, freespacebyte, rommapper, romdata), rommapper);
}



int ratsstart(int snesaddr, const unsigned char * romdata, mapper_t rommapper)
{
	int pcaddr=snestopc(snesaddr, rommapper);
	if (pcaddr<0x7FFF8) return -1;
	const unsigned char * start=romdata+pcaddr-0x10000;
	for (int i=0x10000;i>=0;i--)
	{
		if (!strncmp((const char*)start+i, "STAR", 4) &&
				(start[i+4]^start[i+6])==0xFF && (start[i+5]^start[i+7])==0xFF)
		{
			if ((start[i+4]|(start[i+5]<<8))>0x10000-i-8-1)
                return pctosnes( (int) (start-romdata+i), rommapper);
			return -1;
		}
	}
	return -1;
}

void resizerats(int snesaddr, int newlen, const unsigned char *romdata, mapper_t rommapper)
{
	int pos=snestopc(ratsstart(snesaddr, romdata), rommapper);
	if (pos<0)
        return;
	if (newlen!=1)
        newlen--;
	writeromdata_byte(pos+4, (unsigned char)(newlen&0xFF), romdata);
	writeromdata_byte(pos+5, (unsigned char)((newlen>>8)&0xFF), romdata);
	writeromdata_byte(pos+6, (unsigned char)((newlen&0xFF)^0xFF), romdata);
	writeromdata_byte(pos+7, (unsigned char)(((newlen>>8)&0xFF)^0xFF), romdata);
}

void removerats(int snesaddr, unsigned char clean_byte, const unsigned char * romdata, mapper_t rommapper)
{
	int addr=ratsstart(snesaddr, romdata);
	if (addr<0)
        return;
	// randomdude999: don't forget bank borders
	WalkMetadata(pctosnes(snestopc(addr, rommapper)+8, rommapper), handleprot, romdata);
	addr=snestopc(addr, rommapper);
	for (int i=(romdata[addr+4]|(romdata[addr+5]<<8))+8;i>=0;i--)
        writeromdata_byte(addr+i, clean_byte, romdata);
}



void WalkRatsTags(const unsigned char * romdata, mapper_t rommapper,
                void(*func)(int loc, int len, const unsigned char * romdata))
{
	int pos=snestopc(0x108000, rommapper);
	while (pos<romlen)
	{
		if (!strncmp((const char*)romdata+pos, "STAR", 4) &&
					(romdata[pos+4]^romdata[pos+6])==0xFF && (romdata[pos+5]^romdata[pos+7])==0xFF)
		{
			func(pctosnes(pos+8, rommapper), (romdata[pos+4]|(romdata[pos+5]<<8))+1, romdata);
			pos+=(romdata[pos+4]|(romdata[pos+5]<<8))+1+8;
		}
		else pos++;
	}
}

void WalkMetadata(const unsigned char * romdata, mapper_t rommapper, int loc, 
            void(*func)(int loc, char * name, int len, const unsigned char * contents, const unsigned char *romdata))
{
	int pcoff=snestopc(loc, rommapper);
	if (strncmp((const char*)romdata+pcoff-8, "STAR", 4)) return;
	const unsigned char * metadata=romdata+pcoff;
	while (isupper(metadata[0]) && isupper(metadata[1]) && isupper(metadata[2]) && isupper(metadata[3]))
	{
		if (!strncmp((const char*)metadata, "STOP", 4))
		{
			metadata=romdata+pcoff;
			while (isupper(metadata[0]) && isupper(metadata[1]) && isupper(metadata[2]) && isupper(metadata[3]))
			{
				if (!strncmp((const char*)metadata, "STOP", 4))
					break;
				func(pctosnes((int)(metadata-romdata), rommapper), 
                    (char*)const_cast<unsigned char*>(metadata), 
                    metadata[4], metadata+5, romdata);
				metadata+=5+metadata[4];
			}
			break;
		}
		metadata+=5+metadata[4];
	}
}



bool goodchecksum(const unsigned char *romdata, mapper_t rommapper)
{
	int checksum = (int) getchecksum(romdata);
	return ((romdata[snestopc(0x00FFDE, rommapper)] ^ romdata[snestopc(0x00FFDC, rommapper)])==0xFF) && 
            ((romdata[snestopc(0x00FFDF, rommapper)] ^ romdata[snestopc(0x00FFDD, rommapper)])==0xFF) &&
			((romdata[snestopc(0x00FFDE, rommapper)]&0xFF) == (checksum&0xFF)) &&
            ((romdata[snestopc(0x00FFDF, rommapper)]&0xFF) == ((checksum>>8)&0xFF));
}

void fixchecksum(const unsigned char *romdata, mapper_t rommapper)
{
	// randomdude999: clear out checksum bytes before recalculating checksum, this should make it correct on roms that don't have a checksum yet
	writeromdata(snestopc(0x00FFDC, rommapper), "\xFF\xFF\0\0", 4, romdata);
	int checksum= (int) getchecksum(romdata);
	writeromdata_byte(snestopc(0x00FFDE, rommapper), (unsigned char)(checksum&255), romdata);
	writeromdata_byte(snestopc(0x00FFDF, rommapper), (unsigned char)((checksum>>8)&255), romdata);
	writeromdata_byte(snestopc(0x00FFDC, rommapper), (unsigned char)((checksum&255)^255), romdata);
	writeromdata_byte(snestopc(0x00FFDD, rommapper), (unsigned char)(((checksum>>8)&255)^255), romdata);
}

