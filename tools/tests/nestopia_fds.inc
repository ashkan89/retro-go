// Original synthetic BIOS/disk fixture. No commercial BIOS or game data.
#include <NstApiFds.hpp>
#ifdef RG_NESTOPIA_READ_BANK_SUPPORTED
struct FdsReadProbe {
    Nes::Core::Io::Port original;
    static Nes::Core::Data NST_REGCALL read(void *p,Nes::Core::Address a) {
        return static_cast<FdsReadProbe*>(p)->original.Peek(a)^0x80;
    }
    static void NST_REGCALL write(void*,Nes::Core::Address,Nes::Core::Data) {}
};
#endif
static void fds_regression() {
    uint64_t videoHash=14695981039346656037ULL,audioHash=videoHash,stateHash=videoHash;
    auto hash=[](uint64_t &h,const void *data,size_t n) {
        auto p=static_cast<const unsigned char*>(data);
        while(n--) { h^=*p++; h*=1099511628211ULL; }
    };
    for(unsigned scene=0;scene<2;++scene) {
        std::string bios(8192,char(0xea));
        const unsigned char boot[]={0x78,0xd8,0xa2,0xff,0x9a,0x4c,0x00,0x60};
        memcpy(&bios[0],boot,sizeof(boot));
        const unsigned char nmiJump[]={0x4c,0x00,0x62},irqJump[]={0x4c,0x00,0x63};
        memcpy(&bios[0x100],nmiJump,3); memcpy(&bios[0x103],irqJump,3);
        const unsigned vectors[]={0xe100,0xe000,0xe103};
        for(unsigned i=0;i<3;++i) { bios[8192-6+i*2]=char(vectors[i]); bios[8192-5+i*2]=char(vectors[i]>>8); }
        std::istringstream biosStream(bios);
        Nes::Api::Emulator emulator; Nes::Api::Machine machine(emulator); Nes::Api::Fds fds(emulator);
        assert(fds.SetBIOS(&biosStream)>=0);
        std::string disk(16+2*65500,'\0'); memcpy(&disk[0],"FDS\x1a",4); disk[4]=2;
        for(unsigned side=0;side<2;++side) {
            unsigned base=16+side*65500; disk[base]=1; memcpy(&disk[base+1],"*NINTENDO-HVC*",14);
            memcpy(&disk[base+15],"TEST",4); disk[base+21]=side; disk[base+56]=2;
            for(unsigned i=58;i<65500;++i) disk[base+i]=char((i*13)^(i>>3)^side*0x5a);
        }
        std::istringstream input(disk);
        Nes::Api::User::fileIoCallback.Unset();
        assert(machine.Load(input,Nes::Api::Machine::FAVORED_FAMICOM)>=0);
        assert(machine.Is(Nes::Api::Machine::DISK));
        assert(machine.SetMode(machine.GetDesiredMode())>=0);
        Nes::Api::Input controllers(emulator); assert(controllers.AutoSelectControllers()>=0);
        if(scene) assert(controllers.ConnectController(1,Nes::Api::Input::ZAPPER)>=0);
        Nes::Api::Video video(emulator); Nes::Api::Video::RenderState state;
        state.width=256; state.height=240; state.bits.count=16;
        state.bits.mask.r=0xf800; state.bits.mask.g=0x07e0; state.bits.mask.b=0x1f;
        assert(video.SetRenderState(state)>=0); assert(video.EnableUnlimSprites(scene)>=0);
        assert(Nes::Api::Sound(emulator).SetSampleRate(32000)>=0);
        assert(machine.Power(true)>=0); assert(fds.InsertDisk(0,0)>=0);
        auto &core=static_cast<Nes::Core::Machine&>(emulator); auto &cpu=core.cpu;
        for(unsigned a=0x6000;a<0xe000;++a) cpu.Poke(a,(a*17)^(a>>7));
        cpu.Poke(0,0); cpu.Poke(0x7003,0);
        for(unsigned i=0;i<8192;++i) core.ppu.GetChrMem().Source().Mem()[i]=(i*13)^(i>>3);
        std::vector<unsigned char> code;
        auto emit=[&](std::initializer_list<unsigned char> b){code.insert(code.end(),b);};
        auto imm=[&](unsigned op,unsigned v){emit({static_cast<unsigned char>(op),static_cast<unsigned char>(v)});};
        auto abs=[&](unsigned op,unsigned a){emit({static_cast<unsigned char>(op),static_cast<unsigned char>(a),static_cast<unsigned char>(a>>8)});};
        auto write=[&](unsigned a,unsigned v){imm(0xa9,v);abs(0x8d,a);};
        auto branch=[&](unsigned op,size_t label){int d=int(label)-int(code.size()+2); assert(d>=-128&&d<=127);emit({static_cast<unsigned char>(op),static_cast<unsigned char>(d)});};
        write(0x2000,0);write(0x2001,0);write(0x4010,0);write(0x4015,0);write(0x4017,0x40);
        size_t wait=code.size();abs(0x2c,0x2002);branch(0x10,wait);
        write(0x2006,0x3f);write(0x2006,0);imm(0xa2,0);
        size_t colors=code.size();emit({0x8a});abs(0x8d,0x2007);emit({0xe8});imm(0xe0,32);branch(0xd0,colors);
        write(0x2006,0x20);write(0x2006,0);imm(0xa0,4);imm(0xa2,0);
        size_t tiles=code.size();emit({0x8a});abs(0x8d,0x2007);abs(0x9d,0x0200);emit({0xe8});branch(0xd0,tiles);emit({0x88});branch(0xd0,tiles);
        write(0x4023,3);write(0x4089,0x80);imm(0xa2,0);
        size_t wave=code.size();emit({0x8a});abs(0x9d,0x4040);emit({0xe8});imm(0xe0,64);branch(0xd0,wave);
        write(0x4089,0);write(0x4080,0x9f);write(0x4082,0x80);write(0x4083,1);write(0x408a,0xe8);
        write(0x4087,0x80);imm(0xa2,0);size_t mod=code.size();emit({0x8a});imm(0x29,7);abs(0x8d,0x4088);emit({0xe8});imm(0xe0,32);branch(0xd0,mod);
        write(0x4085,0);write(0x4084,0x88);write(0x4086,0x80);write(0x4087,1);
        write(0x4020,0xf3);write(0x4021,3);write(0x4022,3);write(0x4025,0x85);
        write(0x4015,1);write(0x4000,0xbf);write(0x4002,0x80);write(0x4003,8);
        write(0x2005,0);write(0x2005,0);write(0x2000,0x80);write(0x2001,0x1e);emit({0x58});
        unsigned loop=0x6000+code.size();
        // Execute self-modifying code in the lower FDS RAM and read all three
        // upper RAM banks plus the fixed BIOS through their real CPU handlers.
        abs(0xee,0x8001);abs(0xad,0x8001);abs(0x4d,0xa123);abs(0x4d,0xc123);abs(0x4d,0xe000);abs(0x8d,0x7000);
        abs(0xee,0x8101);abs(0x20,0x8100);abs(0x8d,0x7007);
        abs(0x4c,loop); assert(code.size()<0x200);
        for(unsigned i=0;i<code.size();++i) cpu.Poke(0x6000+i,code[i]);
        cpu.Poke(0x8100,0xa9);cpu.Poke(0x8101,0);cpu.Poke(0x8102,0x60);
        code.clear();emit({0x48,0x8a,0x48,0xe6,0x00});write(0x2003,0);write(0x4014,2);
        emit({0xa5,0x00});abs(0x8d,0x2005);write(0x2005,0);
        emit({0xa5,0x00});imm(0x29,0xe0);imm(0x09,0x1e);abs(0x8d,0x2001);
        emit({0xa5,0x00});abs(0x8d,0x4082);abs(0x8d,0x4024);abs(0x8d,0x4026);
        imm(0x29,4);imm(0x09,0x81);abs(0x8d,0x4025);
        abs(0xad,0x4031);abs(0x8d,0x7001);abs(0xad,0x4032);abs(0x8d,0x7002);
        abs(0xad,0x4033);abs(0x8d,0x7004);write(0x4016,1);write(0x4016,0);
        abs(0xad,0x4016);abs(0x8d,0x7005);abs(0xad,0x4017);abs(0x8d,0x7006);
        emit({0x68,0xaa,0x68,0x40});assert(code.size()<0x100);
        for(unsigned i=0;i<code.size();++i) cpu.Poke(0x6200+i,code[i]);
        const unsigned char irq[]={0x48,0xad,0x30,0x40,0xee,0x03,0x70,0x68,0x40};
        for(unsigned i=0;i<sizeof(irq);++i) cpu.Poke(0x6300+i,irq[i]);
#ifdef RG_NESTOPIA_READ_BANK_SUPPORTED
        assert(cpu.GetDirectReadPageCount()==128);
#endif
        std::vector<uint16_t> pixels(256*240);int16_t pcm[643]; Nes::Core::Input::Controllers pads;
        for(unsigned frame=0;frame<720;++frame) {
            if(frame==300) assert(fds.EjectDisk()>=0);
            if(frame==320) assert(fds.InsertDisk(0,1)>=0);
            if(frame==560) assert(fds.InsertDisk(0,0)>=0);
            if(frame==400) {std::stringstream saved;assert(machine.SaveState(saved,Nes::Api::Machine::NO_COMPRESSION)>=0);assert(machine.LoadState(saved)>=0);}
#ifdef RG_TEST_DIRECT_RGB
            assert(video.PrepareRgbFrame()==(scene?Nes::RESULT_NOP:Nes::RESULT_OK));
#endif
#ifdef RG_NESTOPIA_READ_BANK_SUPPORTED
            assert(cpu.GetDirectReadPageCount()==128);
#endif
            pads.pad[0].buttons=(frame/30)&1;
            std::fill(pcm,pcm+643,12345);Nes::Core::Sound::Output sound(pcm+1,641);
            Nes::Core::Video::Output picture(pixels.data(),512);
            assert(emulator.Execute(nullptr,&sound,&pads)>=0);assert(video.Blit(picture)>=0);
            assert(pcm[0]==12345 && pcm[642]==12345 && sound.length[0]<=641);
            if(frame>=2) {hash(videoHash,pixels.data(),pixels.size()*2);hash(audioHash,pcm+1,sound.length[0]*2);}
        }
        assert(cpu.Peek(0)!=0 && cpu.Peek(0x7003)!=0);
        std::stringstream saved;assert(machine.SaveState(saved,Nes::Api::Machine::NO_COMPRESSION)>=0);
        const std::string bytes=saved.str();hash(stateHash,bytes.data(),bytes.size());
        std::ofstream("fds-"+std::to_string(scene)+".state",std::ios::binary).write(bytes.data(),bytes.size());
#ifdef RG_NESTOPIA_READ_BANK_SUPPORTED
        const unsigned expected=cpu.Peek(0x8123);
        FdsReadProbe probe;probe.original=cpu.Map(0x8123);
        cpu.Map(0x8123)=Nes::Core::Io::Port(&probe,&FdsReadProbe::read,&FdsReadProbe::write);
        assert(cpu.GetDirectReadPageCount()==127 && cpu.Peek(0x8123)==(expected^0x80));
        assert(machine.Reset(false)>=0);
        assert(cpu.GetDirectReadPageCount()==128 && cpu.Peek(0x8123)==expected);
#endif
        assert(machine.Power(false)>=0);assert(machine.Unload()>=0);
    }
    printf("FDS video/audio/state fingerprints: %016llx %016llx %016llx\n",
           (unsigned long long)videoHash,(unsigned long long)audioHash,(unsigned long long)stateHash);
}
