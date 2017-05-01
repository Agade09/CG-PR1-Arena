#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <array>
#include <random>
#include <chrono>
#include <omp.h>
#include <limits>
#include <algorithm>
#include <map>
#include <thread>
#include <csignal>
using namespace std;
using namespace std::chrono;

constexpr bool Debug_AI{false},Timeout{false};
constexpr int PIPE_READ{0},PIPE_WRITE{1};
constexpr int N{2};//Number of players, 1v1
constexpr double FirstTurnTime{1*(Timeout?1:10)},TimeLimit{0.05*(Timeout?1:10)};
constexpr int MapSize{154},LinkCount{301};
constexpr char MapLinks[]{"2 3 34 35 50 51 98 99 114 115 130 131 146 147 50 54 2 7 50 55 114 120 98 105 34 42 98 106 66 76 66 77 82 93 82 94 18 44 18 51 3 4 19 20 35 36 83 84 99 100 115 116 131 132 3 7 51 55 3 8 51 56 115 120 147 152 115 121 99 106 19 27 99 107 67 78 83 95 4 5 20 21 52 53 68 69 84 85 100 101 116 117 132 133 4 8 4 9 116 121 116 122 20 27 52 59 100 107 20 28 100 108 68 79 84 95 84 96 5 6 21 22 69 70 85 86 101 102 117 118 149 150 5 9 5 10 117 122 37 43 53 59 117 123 21 28 101 108 21 29 101 109 69 79 69 80 85 96 22 23 54 55 86 87 102 103 118 119 6 10 134 138 6 11 118 123 54 60 118 124 22 29 54 61 102 109 102 110 70 80 70 81 39 40 7 8 55 56 71 72 87 88 135 136 135 139 39 44 119 124 135 140 7 13 55 61 119 125 7 14 55 62 103 110 103 111 71 83 24 25 8 9 40 41 72 73 120 121 152 153 40 44 136 140 40 45 8 14 24 30 56 62 8 15 56 63 120 128 104 113 120 129 72 83 72 84 9 10 25 26 41 42 73 74 105 106 121 122 41 45 137 141 25 30 137 142 9 15 25 31 9 16 89 97 121 129 105 114 121 130 57 67 73 84 73 85 10 11 74 75 90 91 106 107 122 123 138 139 26 31 10 16 26 32 138 144 10 17 90 98 106 114 122 130 90 99 106 115 122 131 58 68 74 85 74 86 11 12 27 28 75 76 91 92 107 108 123 124 139 140 43 46 11 17 139 145 91 99 107 115 123 131 91 100 107 116 123 132 27 37 59 69 59 70 75 86 75 87 44 45 28 29 60 61 76 77 92 93 108 109 124 125 140 145 140 146 92 100 108 116 124 132 92 101 108 117 124 133 28 38 60 71 76 87 76 88 13 14 61 62 93 94 109 110 125 126 141 142 13 19 13 20 141 148 93 101 109 117 125 133 29 38 93 102 109 118 61 71 61 72 77 88 14 15 30 31 46 47 62 63 110 111 126 127 46 48 46 49 14 20 142 148 14 21 94 102 110 118 30 39 94 103 110 119 62 72 62 73 78 89 15 16 31 32 63 64 79 80 95 96 111 112 47 49 15 21 15 22 127 134 143 150 31 39 111 119 31 40 63 73 63 74 79 90 79 91 16 17 0 1 32 33 48 49 64 65 80 81 128 129 16 22 16 23 144 151 32 40 32 41 128 137 64 74 64 75 80 91 80 92 1 2 33 34 65 66 81 82 129 130 145 146 1 3 17 23 97 104 33 41 129 137 33 42 65 75 65 76 81 92 81 93"};

bool stop{false};//Global flag to stop all arena threads when SIGTERM is received

struct cell{
    int plat,owner;
    vector<int> L;
    vector<int> pods;
};

struct player{
    int plat;
};

struct state{
    vector<cell> C;
    vector<player> P;
};

struct play_move{
    int from,to,amount;
};

struct play_buy{
    int from,amount;
};

struct strat{
    vector<play_move> MV;
    vector<play_buy> B;
};

inline string EmptyPipe(const int fd){
    int nbytes;
    if(ioctl(fd,FIONREAD,&nbytes)<0){
        throw(4);
    }
    string out;
    out.resize(nbytes);
    if(read(fd,&out[0],nbytes)<0){
        throw(4);
    }
    return out;
}

struct AI{
    int id,pid,outPipe,errPipe,inPipe;
    string name;
    inline void stop(){
        if(alive()){
            kill(pid,SIGTERM);
            int status;
            waitpid(pid,&status,0);//It is necessary to read the exit code for the process to stop
            if(!WIFEXITED(status)){//If not exited normally try to "kill -9" the process
                kill(pid,SIGKILL);
            }
        }
    }
    inline bool alive()const{
        return kill(pid,0)!=-1;//Check if process is still running
    }
    inline void Feed_Inputs(const string &inputs){
        if(write(inPipe,&inputs[0],inputs.size())!=inputs.size()){
            throw(5);
        }
    }
    inline ~AI(){
        close(errPipe);
        close(outPipe);
        close(inPipe);
        stop();
    }
};

void StartProcess(AI &Bot){
    int StdinPipe[2];
    int StdoutPipe[2];
    int StderrPipe[2];
    if(pipe(StdinPipe)<0){
        perror("allocating pipe for child input redirect");
    }
    if(pipe(StdoutPipe)<0){
        close(StdinPipe[PIPE_READ]);
        close(StdinPipe[PIPE_WRITE]);
        perror("allocating pipe for child output redirect");
    }
    if(pipe(StderrPipe)<0){
        close(StderrPipe[PIPE_READ]);
        close(StderrPipe[PIPE_WRITE]);
        perror("allocating pipe for child stderr redirect failed");
    }
    int nchild{fork()};
    if(nchild==0){//Child process
        if(dup2(StdinPipe[PIPE_READ],STDIN_FILENO)==-1){// redirect stdin
            perror("redirecting stdin");
            return;
        }
        if(dup2(StdoutPipe[PIPE_WRITE],STDOUT_FILENO)==-1){// redirect stdout
            perror("redirecting stdout");
            return;
        }
        if(dup2(StderrPipe[PIPE_WRITE],STDERR_FILENO)==-1){// redirect stderr
            perror("redirecting stderr");
            return;
        }
        close(StdinPipe[PIPE_READ]);
        close(StdinPipe[PIPE_WRITE]);
        close(StdoutPipe[PIPE_READ]);
        close(StdoutPipe[PIPE_WRITE]);
        close(StderrPipe[PIPE_READ]);
        close(StderrPipe[PIPE_WRITE]);
        execl(Bot.name.c_str(),Bot.name.c_str(),(char*)NULL);//(char*)Null is really important
        //If you get past the previous line its an error
        perror("exec of the child process");
    }
    else if(nchild>0){//Parent process
        close(StdinPipe[PIPE_READ]);//Parent does not read from stdin of child
        close(StdoutPipe[PIPE_WRITE]);//Parent does not write to stdout of child
        close(StderrPipe[PIPE_WRITE]);//Parent does not write to stderr of child
        Bot.inPipe=StdinPipe[PIPE_WRITE];
        Bot.outPipe=StdoutPipe[PIPE_READ];
        Bot.errPipe=StderrPipe[PIPE_READ];
        Bot.pid=nchild;
    }
    else{//failed to create child
        close(StdinPipe[PIPE_READ]);
        close(StdinPipe[PIPE_WRITE]);
        close(StdoutPipe[PIPE_READ]);
        close(StdoutPipe[PIPE_WRITE]);
        perror("Failed to create child process");
    }
}

inline bool IsValidMove(const state &S,const AI &Bot,const string &M){
    return count(M.begin(),M.end(),'\n')==2;//1 buy line 1 move line
}

string GetMove(const state &S,AI &Bot,const int turn){
    pollfd outpoll{Bot.outPipe,POLLIN};
    time_point<system_clock> Start_Time{system_clock::now()};
    string out;
    while(static_cast<duration<double>>(system_clock::now()-Start_Time).count()<(turn==1?FirstTurnTime:TimeLimit) && !IsValidMove(S,Bot,out)){
        double TimeLeft{(turn==1?FirstTurnTime:TimeLimit)-static_cast<duration<double>>(system_clock::now()-Start_Time).count()};
        if(poll(&outpoll,1,TimeLeft)){
            out+=EmptyPipe(Bot.outPipe);
        }
    }
    return out;
}

inline bool Has_Won(const array<AI,N> &Bot,const int idx)noexcept{
    if(!Bot[idx].alive()){
        return false;
    }
    for(int i=0;i<N;++i){
        if(i!=idx && Bot[i].alive()){
            return false;
        }
    }
    return true;
}

inline bool All_Dead(const array<AI,N> &Bot)noexcept{
    for(const AI &b:Bot){
        if(b.alive()){
            return false;
        }
    }
    return true;
}

strat StringToStrat(const state &S,const string &M_Str){
    strat M;
    stringstream ss(M_Str);
    string move_line,buy_line;
    getline(ss,move_line);
    getline(ss,buy_line);
    stringstream ss_move(move_line),ss_buy(buy_line),ss_move_waitcheck(move_line),ss_buy_waitcheck(buy_line);
    string first;
    ss_move_waitcheck >> first;
    if(first!="WAIT"){
        while(ss_move){
            play_move mv;
            ss_move >> mv.amount >> mv.from >> mv.to;
            M.MV.push_back(mv);
        }
    }
    ss_buy_waitcheck >> first;
    if(first!="WAIT"){
        while(ss_buy){
            play_buy mv;
            ss_buy >> mv.amount >> mv.from;
            M.B.push_back(mv);
        }
    }
    return M;
}

inline bool ValidCellIndex(const int idx)noexcept{
    return idx>=0 && idx<MapSize;
}

void Simulate(state &S,const array<strat,2> &M){
    for(int i=0;i<N;++i){//Moving
        for(const play_move &mv:M[i].MV){
            if(ValidCellIndex(mv.from) && ValidCellIndex(mv.to) && find_if(S.C[mv.from].L.begin(),S.C[mv.from].L.end(),[&](const int l){return l==mv.to;})!=S.C[mv.from].L.end()){//The link exists
                int presence=count_if(S.C[mv.from].pods.begin(),S.C[mv.from].pods.end(),[&](const int a){return a>0;});
                if(presence<2 || S.C[mv.to].owner==i || S.C[mv.to].owner==-1){//Can only flee battle to own/neutral territory
                    int to_send{min(mv.amount,S.C[mv.from].pods[i])};
                    S.C[mv.from].pods[i]-=to_send;
                    S.C[mv.to].pods[i]+=to_send;
                }
            }
        }
    }
    for(int i=0;i<N;++i){//Buying
        for(const play_buy &mv:M[i].B){
            if(ValidCellIndex(mv.from) && (S.C[mv.from].owner==i || S.C[mv.from].owner==-1) ){
                int to_buy{min(mv.amount,S.P[i].plat/20)};
                S.C[mv.from].pods[i]+=to_buy;
                S.P[i].plat-=to_buy*20;
            }
        }
    }
    for(cell &c:S.C){//Fighting
        const int presence=count_if(c.pods.begin(),c.pods.end(),[&](const int a){return a>0;});
        if(presence>=2){
            vector<int> P=c.pods;
            sort(P.begin(),P.end(),[](const int a,const int b){return a>b;});
            const int losses{min(3,P[1])};
            for(int i=0;i<N;++i){
                c.pods[i]=max(0,c.pods[i]-losses);
                P[i]=max(0,P[i]-losses);
            }
            if(P[1]==0 && P[0]>0){//ownership change
                c.owner=distance(c.pods.begin(),find_if(c.pods.begin(),c.pods.end(),[&](const int a){return a>0;}));
            }
        }
        else if(presence==1 && c.owner==-1){
            c.owner=distance(c.pods.begin(),find_if(c.pods.begin(),c.pods.end(),[&](const int a){return a>0;}));
        }
    }
    for(int i=0;i<N;++i){//Income
        S.P[i].plat+=accumulate(S.C.begin(),S.C.end(),0,[&](const int income,const cell &c){return income+c.plat;});
    }
}

int Play_Game(const array<string,N> &Bot_Names,state &S){
    array<AI,N> Bot;
    for(int i=0;i<N;++i){
        Bot[i].id=i;
        Bot[i].name=Bot_Names[i];
        StartProcess(Bot[i]);
        stringstream ss;
        ss << N << " " << i << " " << S.C.size() << " " << LinkCount << endl;
        for(int z=0;z<S.C.size();++z){
            ss << z << " " << S.C[z].plat << endl;
        }
        stringstream ss_links(MapLinks);
        for(int i=0;i<LinkCount;++i){
            int z1,z2;
            ss_links >> z1 >> z2;
            ss << z1 << " " << z2 << endl;
        }
        Bot[i].Feed_Inputs(ss.str());
    }
    int turn{0};
    while(++turn>0 && !stop){
        array<strat,2> M;
        for(int i=0;i<N;++i){
            if(Bot[i].alive()){
                stringstream ss;
                ss << S.P[i].plat << endl;
                for(int z=0;z<S.C.size();++z){
                    ss << z << " " << S.C[z].owner << " " << S.C[z].pods[0] << " " << S.C[z].pods[1] << " " << S.C[z].pods[2] << " " << S.C[z].pods[3] << endl;
                }
                try{
                    Bot[i].Feed_Inputs(ss.str());
                    M[i]=StringToStrat(S,GetMove(S,Bot[i],turn));
                }
                catch(int ex){
                    if(ex==1){//Timeout
                        cerr << "Loss by Timeout of AI " << Bot[i].id << " name: " << Bot[i].name << endl;
                    }
                    else if(ex==5){
                        cerr << "AI " << Bot[i].name << " died before being able to give it inputs" << endl;
                    }
                    Bot[i].stop();
                }
            }
        }
        for(int i=0;i<2;++i){
            string err_str{EmptyPipe(Bot[i].errPipe)};
            if(Debug_AI){
                ofstream err_out("log.txt",ios::app);
                err_out << err_str << endl;
            }
            if(Has_Won(Bot,i)){
                //cerr << i << " has won in " << turn << " turns" << endl;
                return i;
            }
        }
        if(All_Dead(Bot)){
            return -1;
        }
        Simulate(S,M);
        if(turn==200){
            array<int,N> Zones;
            for(int i=0;i<N;++i){
                Zones[i]=count_if(S.C.begin(),S.C.end(),[&](const cell &c){return c.owner==i;});
            }
            //cerr << "Zones: " << Zones[0] << " " << Zones[1] << endl;
            return Zones[0]>Zones[1]?0:Zones[1]>Zones[0]?1:-1;
        }
    }
    return -2;
}

int Play_Round(array<string,N> Bot_Names){
    default_random_engine generator(system_clock::now().time_since_epoch().count());
    uniform_int_distribution<int> Swap_Distrib(0,1);
    const bool player_swap{Swap_Distrib(generator)==1};
    if(player_swap){
        swap(Bot_Names[0],Bot_Names[1]);
    }
    //Initial state generation
    state S;
    S.C.resize(MapSize);
    stringstream ss(MapLinks);
    while(ss){
        int z1,z2;
        ss >> z1 >> z2;
        S.C[z1].L.push_back(z2);
        S.C[z2].L.push_back(z1);
    }
    for(cell &c:S.C){
        c.pods.resize(N);
        c.owner=-1;
    }
    uniform_int_distribution<int> TotalPlatDistrib(110,120),PlatDistrib(1,6);
    int PlatToAllocate{TotalPlatDistrib(generator)};
    vector<int> Cells(MapSize);
    iota(Cells.begin(),Cells.end(),0);
    shuffle(Cells.begin(),Cells.end(),mt19937{random_device{}()});
    for(int i=0;i<Cells.size() && PlatToAllocate>0;++i){
        const int plat{min(PlatToAllocate,PlatDistrib(generator))};
        S.C[Cells[i]].plat=plat;
        PlatToAllocate-=plat;
    }
    S.P.resize(N);
    for(player& p:S.P){
        p.plat=200;
    }
    int winner{Play_Game(Bot_Names,S)};
    if(player_swap){
        return winner==-1?-1:winner==0?1:0;
    }
    else{
        return winner;
    }
}

void StopArena(const int signum){
    stop=true;
}

int main(int argc,char **argv){
    if(argc<3){
        cerr << "Program takes 2 inputs, the names of the AIs fighting each other" << endl;
        return 0;
    }
    int N_Threads{1};
    if(argc>=4){//Optional N_Threads parameter
        N_Threads=min(2*omp_get_num_procs(),max(1,atoi(argv[3])));
        cerr << "Running " << N_Threads << " arena threads" << endl;
    }
    array<string,N> Bot_Names;
    for(int i=0;i<2;++i){
        Bot_Names[i]=argv[i+1];
    }
    cout << "Testing AI " << Bot_Names[0];
    for(int i=1;i<N;++i){
        cerr << " vs " << Bot_Names[i];
    }
    cerr << endl;
    for(int i=0;i<N;++i){//Check that AI binaries are present
        ifstream Test{Bot_Names[i].c_str()};
        if(!Test){
            cerr << Bot_Names[i] << " couldn't be found" << endl;
            return 0;
        }
        Test.close();
    }
    signal(SIGTERM,StopArena);//Register SIGTERM signal handler so the arena can cleanup when you kill it
    signal(SIGPIPE,SIG_IGN);//Ignore SIGPIPE to avoid the arena crashing when an AI crashes
    int games{0},draws{0};
    array<double,2> points{0,0};
    #pragma omp parallel num_threads(N_Threads) shared(games,points,Bot_Names)
    while(!stop){
        int winner{Play_Round(Bot_Names)};
        if(winner==-1){//Draw
            #pragma omp atomic
            ++draws;
            #pragma omp atomic
            points[0]+=0.5;
            #pragma omp atomic
            points[1]+=0.5;
        }
        else{//Win
            ++points[winner];
        }
        #pragma omp atomic
        ++games;
        double p{static_cast<double>(points[0])/games};
        double sigma{sqrt(p*(1-p)/games)};
        double better{0.5+0.5*erf((p-0.5)/(sqrt(2)*sigma))};
        #pragma omp critical
        cout << "Wins:" << setprecision(4) << 100*p << "+-" << 100*sigma << "% Rounds:" << games << " Draws:" << draws << " " << better*100 << "% chance that " << Bot_Names[0] << " is better" << endl;
    }
}