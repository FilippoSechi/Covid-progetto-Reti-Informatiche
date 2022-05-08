#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

#define IP "127.0.0.12"
#define BUF_SIZE 100
#define MSG_SIZE 9      /*lunghezza messaggi inviati al DS */
#define DS_MSG_SIZE 4   /* lunghezza messaggi ricevuti dal DS */
#define DATA_LEN 11     /* lunghezza dalla data nel formato yyyy:mm:dd */

struct entry{
    int quantita;
    struct entry* pros;
    char tipo;
    int ricevuta;   /* indica se la entry è stata ricevuta da un altro peer--> 1 = entry ricevuta; 0 = entry locale */
};
/* struttura contentente i risultati dei dati aggregati */
struct aggregato{
    int finale_TOT; /* TOTALE-> 0 = dato non calcolato; 1 = dato già calcolato */
    int finale_VAR; /* VARIAZIONE-> 0 = dato non calcolato; 1 = dato già calcolato */
    int TOT;    /* valido solo se finale_TOT == 1 */
    int VAR;    /* valido solo se finale_VAR == 1 */
};

struct registro{
    char data_reg[DATA_LEN];
    struct entry* prima_entry;
    struct entry* ultima_entry;
    struct aggregato aggr_T;    /* dati elaborati per le entries giornaliere di tipo T */
    struct aggregato aggr_N;     /* dati elaborati per le entries giornaliere di tipo N */
    struct registro* pros;
    int tot_T;                  /* totale entries di tipo T salvate localmente */
    int tot_N;                  /* totale entries di tipo N salvate localmente */
};

void help(){
    printf("*************PEER COVID*********\n");
    printf("Digita un comando:\n");
    printf("1)help --> mostra i dettagli dei comandi\n");
    printf("2)start DS_IP DS_PORT --> connette il peer al DS in ascolto all'indirizzo DS_IP:DS_PORT\n");
    printf("3)add type N --> inserisce una entry di tipo <type> e quantità <N> (i tipi possibili sono N e T)\n");
    printf("4)get ag type p1 p2 --> permette di ottenere il dato aggregato <ag>(TOT o VAR) delle entry di tipo <type>(T o N) nel periodo <p1-p2>\n");
    printf("5)esc --> chiude il peer\n");
    printf("---\n");
    return ;
}

/* Inserisce la data corrente in una stringa */
void dataStringa(char* stringa, struct tm* data){
    sprintf(stringa,"%04d:%02d:%02d",data->tm_year+1900,data->tm_mon+1,data->tm_mday);
    return;
}

/* aggiunge un giorno alla data odierna */
void aggiungiGiorno(struct tm* data){
    data->tm_mday += 1;
    mktime(data);
    return;
}

/* inizializza un nuovo registro */
void nuovo_registro(struct registro* reg){
    reg->prima_entry = reg->ultima_entry = NULL;
    reg->pros = NULL;
    reg->aggr_N.finale_TOT = reg->aggr_N.finale_VAR = 0;    /* i dati aggregati non sono validi inizialmente*/
    reg->aggr_T.finale_VAR = reg->aggr_T.finale_TOT = 0;
    reg->tot_N = reg->tot_T = 0;
    return;
}

/* apre un nuovo registro se necessario (se sono le 18:00) */
void aggiornaRegistro(struct registro** reg,struct tm* data){
    char stringa[DATA_LEN];
    int res;

    dataStringa(stringa,data);
    res = strcmp(stringa,(*reg)->data_reg);
    if(res < 0) return; /* registro del giorno successivo già aperto */
    if(!res && data->tm_hour < 18)  return; /* non sono ancora le 18 */

    /* apro il nuovo registro */
    if(!res && data->tm_hour >= 18){
        (*reg)->pros = malloc(sizeof(struct registro));
        *reg = (*reg)->pros;
        nuovo_registro(*reg);    /* inizializzo il nuovo registro */
        aggiungiGiorno(data);
        dataStringa((*reg)->data_reg,data);  /* inserisco la nuova data */
        return;
    }
}


/* conta il totale delle entries di un certo tipo (T,N) in un registro */
int conta(struct entry* scroll, char tipo){
    int totale = 0;
    if(!scroll) return totale;
    while(scroll){
        if(scroll->tipo == tipo)
            totale += scroll->quantita;
        scroll = scroll->pros;
    }
    return totale;
}

/* Inserisce una entry nel registro corrente */
int inserisciEntry(struct registro** reg,char tipo, int quanti,int ricevuta){
    if(tipo != 'N' && tipo != 'T') return 0;
    if(quanti <= 0) return 0;
    struct entry* ent = malloc(sizeof(struct entry));
    ent->pros = NULL;
    ent->quantita = quanti;
    ent->tipo = tipo;
    ent->ricevuta = ricevuta;
    if(tipo == 'N') 
        (*reg)->tot_N++;
    else    
        (*reg)->tot_T++;

    if((*reg)->ultima_entry){
        (*reg)->ultima_entry->pros = ent;
        (*reg)->ultima_entry = (*reg)->ultima_entry->pros;
        return 1;
    }
    /*inserimento in testa */
    (*reg)->prima_entry = ent;
    (*reg)->ultima_entry = (*reg)->prima_entry;
    return 1;
}
/* Raccoglie i dati necessari (dal DS e dai VICINI) per il calcolo dei dati aggregati */
int richiediTOT(struct registro* reg, char tipo, int sd,struct sockaddr_in* ds,int* sd_TCP,fd_set* MASTER,int mia_porta){
    uint32_t contatore = 0;
    uint32_t dim_msg;  
    int i;
    int ret;
    socklen_t len;
    int totale;
    int temp_sd;    /* usato nella scannerizzazione della risposta a FLOOD_FOR_ENTRIES */
    int porta;      /* usato nella scannerizzazione della risposta a FLOOD_FOR_ENTRIES */
    uint16_t vicini[2];    /* porte dei vicini */
    char stringa[200] = "REQUES ";
    char risposta[200];
    char* token;    /* usata con la strtok() */
    char tipo_stringa[2];   /* trascrivo il char TIPO in stringa (utile per fare strcmp()) */
    struct sockaddr_in peer;
    if(tipo == 'N')
        strcpy(tipo_stringa,"N");
    else
        strcpy(tipo_stringa,"T");

    strcat(stringa,tipo_stringa);   /* concateno la stringa "REQUES N" / "REQUES T" da mandare al DS */

    /* chiedo al DS quante entries ci sono in totale quel giorno */

    sendto(sd,stringa,MSG_SIZE,0,(struct sockaddr*)ds,sizeof(struct sockaddr_in));  /* messaggio REQUES <tipo>*/
    sendto(sd,reg->data_reg,DATA_LEN,0,(struct sockaddr*)ds,sizeof(struct sockaddr_in));    /* invio la data */

    recvfrom(sd,&contatore,sizeof(uint32_t),0,(struct sockaddr*)ds,&len);  /* ricevo il numero totale di entry in quella data */
    contatore = ntohl(contatore);

    if(tipo == 'N')
        printf("Le entry totali sono %d, quelle salvate localmente sono %d\n",contatore,reg->tot_N);
    else
        printf("Le entry totali sono %d, quelle salvate localmente sono %d\n",contatore,reg->tot_T);

    /* controllo se ho tutte le entry necessarie */
    if((tipo == 'N' && reg->tot_N == contatore) || (tipo == 'T' && reg->tot_T == contatore))
        return conta(reg->prima_entry,tipo);    /* possiedo già tutte le entries, posso calcolare il risultato */
    
    /* mancano delle entries */
    else{
        
        for(i = 0; i <= 1; i++){        /* chiedo ad un vicino alla volta */
            if(FD_ISSET(sd_TCP[i],MASTER)){
                /* chiedo ai vicini se hanno il dato calcolato */
                sprintf(stringa,"%s %s %c","REQ_DATA",reg->data_reg,tipo); /* messaggio REQ_DATA <data> <tipo> */
                dim_msg = strlen(stringa) + 1;
                dim_msg = htonl(dim_msg);
                do{
                    ret = send(sd_TCP[i],&dim_msg,sizeof(uint32_t),0);   /* dimensione del messaggio */
                }   while(ret <= 0);
                dim_msg = ntohl(dim_msg);
                do{
                    ret = send(sd_TCP[i],stringa,dim_msg,0); 
                }   while(ret <= 0);        
    
                recv(sd_TCP[i],&dim_msg,sizeof(uint32_t),0);   /* ricevo la DIMENSIONE DELLA RISPOSTA */
                dim_msg = ntohl(dim_msg);
                /* ricevo REPLY_DATA */
                recv(sd_TCP[i],risposta,dim_msg,0);
                sscanf(risposta,"%s %d",stringa,&totale);       /* risposta: REPLY_DATA <dato disponibile> */
                if(!strcmp(stringa,"REPLY_DATA")){
                    if(totale != -1){     /* il vicino aveva il dato già calcolato*/
                        printf("il vicino ha il dato calcolato\n");
                        return totale;
                    }
                }
            }
        }

        if(!FD_ISSET(sd_TCP[0],MASTER) && !FD_ISSET(sd_TCP[1],MASTER)){
            printf("Non ho vicini a cui richiedere le entries\n");
            return conta(reg->prima_entry,tipo);
        }

        /* chiedo al DS le porte dei miei vicini per compararle a quelle che riceverò */
        strcpy(stringa,"MY_NEIGH");
        sendto(sd,stringa,MSG_SIZE,0,(struct sockaddr*)ds,sizeof(struct sockaddr_in));  /* messaggio "MY_NEIGH" */
        mia_porta = htons(mia_porta);
        sendto(sd,&mia_porta,sizeof(uint16_t),0,(struct sockaddr*)ds,sizeof(struct sockaddr_in));   /* invio il mio numero di porta */

        for(i = 0; i<= 1; i++){ /* salvo in vicini[] i numeri di porta ricevuti dal DS come risposta a "MY_NEIGH" */
            recvfrom(sd,&vicini[i],sizeof(uint16_t),0,(struct sockaddr*)ds,&len);
            vicini[i] = ntohs(vicini[i]);
        }

        /* Caso in cui i vicini non avevano il dato calcolato */
        sprintf(stringa,"FLOOD_FOR_ENTRIES %s %c",reg->data_reg,tipo); /* creo la stringa FLOOD_FOR_ENTRIES <data> <tipo> */
        dim_msg = strlen(stringa) + 1;
        dim_msg = htonl(dim_msg);
        /* invio il messaggio ad un vicino, che poi lo passerà agli altri vicini(i quali aggiungono il loro n. di porta) 
            fino a tornare al peer di partenza */
        if(!FD_ISSET(sd_TCP[1],MASTER)){    /* caso particolare con 2 peer nella rete */
            send(sd_TCP[0],&dim_msg,sizeof(uint32_t),0);
            dim_msg = ntohl(dim_msg);
            send(sd_TCP[0],stringa,dim_msg,0);
        }
        else{
            send(sd_TCP[1],&dim_msg,sizeof(uint32_t),0);
            dim_msg = ntohl(dim_msg);
            send(sd_TCP[1],stringa,dim_msg,0);
        }
        printf("INVIATO FLOOD_FOR_ENTRIES\n");

        /* Aspetto la ricezione della risposta */
        if(!FD_ISSET(sd_TCP[1],MASTER)){    /* caso particolare con 2 peer nella rete */
            recv(sd_TCP[0],&dim_msg,sizeof(uint32_t),0);
            dim_msg = ntohl(dim_msg);
            recv(sd_TCP[0],stringa,dim_msg,0);
        }
        else{
            recv(sd_TCP[0],&dim_msg,sizeof(uint32_t),0);
            dim_msg = ntohl(dim_msg);
            recv(sd_TCP[0],stringa,dim_msg,0);
        }

        /* preparo la struct sockaddr_in peer per usarla per contattare i peer che hanno delle entries */
        inet_pton(AF_INET,IP,&peer.sin_addr); 
        peer.sin_family = AF_INET;

        /* Ricavo i numeri di porta dei peer che hanno delle entry */
        token = strtok(stringa," ");
        while(token != NULL){
            /* scarto dalla scansione FLOOD_FOR_ENTRIES, la data e il tipo */
            if(strcmp(token,"FLOOD_FOR_ENTRIES") && strcmp(token,reg->data_reg) && strcmp(token,tipo_stringa)){
                porta = atoi(token);    /* ricavo il numero di porta del peer avente delle entries */

                /* controllo che non sia uno dei miei vicini (con il quale esiste già una connessione) */
                if(porta != vicini[0] && porta != vicini[1]){   
                    temp_sd = socket(AF_INET,SOCK_STREAM,0); /* socket per comunicare con i peer */
                    peer.sin_port = htons(porta);
                    /* connessione con il peer */
                    connect(temp_sd,(struct sockaddr*)&peer,sizeof(peer));
                    
                    strcpy(stringa,"REQUESTER");    /* messaggio di "presentazione" "REQUESTER" al nuovo peer */
                    dim_msg = htonl(strlen(stringa) + 1);
                    send(temp_sd,&dim_msg,sizeof(uint32_t),0);
                    dim_msg = ntohl(dim_msg);
                    send(temp_sd,stringa,dim_msg,0);
                }
                /* caso in cui fosse uno dei vicini */
                else if(porta == vicini[0])
                    temp_sd = sd_TCP[0];
                else if(porta == vicini[1])
                    temp_sd = sd_TCP[1];

                /* Richiedo le entries mancanti */
                sprintf(stringa,"REQ_ENTRIES %s %c",reg->data_reg,tipo);    /* costruisco il messaggio REQ_ENTRIES <data> <tipo> */
                dim_msg = htonl(strlen(stringa) + 1);
                send(temp_sd,&dim_msg,sizeof(uint32_t),0);
                dim_msg = ntohl(dim_msg);
                send(temp_sd,stringa,dim_msg,0);
                printf("INVIATO REQ_ENTRIES\n");

                recv(temp_sd,&dim_msg,sizeof(uint32_t),0);  /* ricevo il numero di entries totali da ricevere da questo peer */
                dim_msg = ntohl(dim_msg);
                for(i = 0; i < dim_msg; i++){   /* salvo le entries una ad una */
                    recv(temp_sd,&contatore,sizeof(uint32_t),0);   
                    contatore = ntohl(contatore);
                    inserisciEntry(&reg,tipo,contatore,1);
                }
                printf("Ricevute %d entries \n",dim_msg);
                
            }
            token = strtok(NULL," ");
        }
        return conta(reg->prima_entry,tipo);
    }
}
/* comando GET */
int get(struct registro* primo,struct registro* ultimo,char* aggr,char tipo, 
        char* periodo1, char* periodo2,int sd,struct sockaddr_in* ds,int* sd_TCP,fd_set* MASTER,int porta){

    struct registro* scroll = primo;
    int totale = 0;
    struct registro* prec;  /* puntatore al registro precedente a quello richiesto (usato in caso di richiesta di dati VAR) */
    if(primo == ultimo)     /* non ci sono registri chiusi */
        return 0;
    /* controllo la validità dei parametri */
    if(tipo != 'N' && tipo != 'T')
        return 0;
    if(strcmp(aggr,"VAR") && strcmp(aggr,"TOT"))
        return 0;

    if(!strcmp(periodo2,"*")){  /* salvo la data relativa all'ultimo registro chiuso */
        while(scroll->pros != ultimo)
            scroll = scroll->pros;
        
        strcpy(periodo2,scroll->data_reg);
    }
    if(!strcmp(periodo1,"*"))   /* salvo la data relativa al primo registro chiuso */
        strcpy(periodo1,primo->data_reg);
    
    if(strcmp(periodo2,ultimo->data_reg) >= 0)  /* data finale non valida */
        return 0;
    if(strcmp(periodo1,primo->data_reg) < 0)    /* data iniziale non valida */
        return 0;
    
    scroll = primo;
    while(scroll){
        if(!strcmp(scroll->data_reg,periodo1))  /* posiziono scroll sul primo registro da analizzare */
            break;
        scroll = scroll->pros;
    }
    if(!scroll) return 0;
    
    while(strcmp(scroll->data_reg,periodo2) <= 0){
        if(!strcmp(aggr,"TOT")){
            /* TOT N */
            if(tipo == 'N'){
                if(scroll->aggr_N.finale_TOT)
                    totale += scroll->aggr_N.TOT;
                else{
                    scroll->aggr_N.TOT = richiediTOT(scroll,tipo,sd,ds,sd_TCP,MASTER,porta);
                    scroll->aggr_N.finale_TOT = 1;
                    totale += scroll->aggr_N.TOT;
                }
            }
            /* TOT T */
            else{
                if(scroll->aggr_T.finale_TOT)
                    totale += scroll->aggr_T.TOT;
                else{
                    scroll->aggr_T.TOT = richiediTOT(scroll,tipo,sd,ds,sd_TCP,MASTER,porta);
                    scroll->aggr_T.finale_TOT = 1;
                    totale += scroll->aggr_T.TOT;
                }
            }        
        }
        else if(!strcmp(aggr,"VAR")){
            /* VAR N */
            if(tipo == 'N'){
                if(scroll->aggr_N.finale_VAR)
                    printf("RISULTATO: %d\n",scroll->aggr_N.VAR);
                else{
                    prec = primo;
                    if(prec != scroll){
                        while(prec->pros != scroll)
                            prec = prec->pros;
                    }
                    if(prec != scroll){
                        /* calcolo il  totale dei due giorni */
                        scroll->aggr_N.TOT = richiediTOT(scroll,tipo,sd,ds,sd_TCP,MASTER,porta);
                        scroll->aggr_N.finale_TOT = 1;
                        prec->aggr_N.TOT = richiediTOT(prec,tipo,sd,ds,sd_TCP,MASTER,porta);
                        prec->aggr_N.finale_TOT = 1;
                        scroll->aggr_N.VAR = scroll->aggr_N.TOT - prec->aggr_N.TOT; 
                        printf("RISULTATO: %d\n",scroll->aggr_N.VAR);
                        scroll->aggr_N.finale_VAR = 1;
                    }
                }
            }
            /* VAR T */
            else{
                if(scroll->aggr_T.finale_VAR)
                    printf("RISULTATO: %d\n",scroll->aggr_T.VAR);
                else{
                    prec = primo;
                    if(prec != scroll){
                        while(prec->pros != scroll)
                            prec = prec->pros;
                    }
                    if(prec != scroll){
                        /* calcolo il  totale dei due giorni */
                        scroll->aggr_T.TOT = richiediTOT(scroll,tipo,sd,ds,sd_TCP,MASTER,porta);
                        scroll->aggr_T.finale_TOT = 1;
                        prec->aggr_T.TOT = richiediTOT(prec,tipo,sd,ds,sd_TCP,MASTER,porta);
                        prec->aggr_T.finale_TOT = 1;
                        scroll->aggr_T.VAR = scroll->aggr_T.TOT - prec->aggr_T.TOT; 
                        printf("RISULTATO: %d\n",scroll->aggr_T.VAR);
                        scroll->aggr_T.finale_VAR = 1;
                    }
                }
            }
        }
        scroll = scroll->pros;
    }
    if(!strcmp(aggr,"TOT"))
        printf("RISULTATO: %d\n",totale);
    return 1;
}
/* Ritorna un puntatore al registro corrispondente ad una data. Ritorna NULL se non esiste */
struct registro* trovaRegistro(struct registro* testa,char* data){
    while(testa){
        if(!strcmp(data,testa->data_reg))
            return testa;
        testa = testa->pros;
    }
    return NULL;
}
/* Invia i messaggi delle entries mancanti ai peer richiedenti */
void REQ_ENTRIES(char* msg_peer,char tipo,int sd,struct registro* scroll){
    uint32_t dim_msg_PEER = 0;
    struct entry* entr;
    int var;
    
    entr = scroll->prima_entry;
    while(entr){
        if(!entr->ricevuta && entr->tipo == tipo)
            dim_msg_PEER++;
        entr = entr->pros;
    }

    dim_msg_PEER = htonl(dim_msg_PEER);
    send(sd,&dim_msg_PEER,sizeof(uint32_t),0);  /* invio il numero di entries totali salvate localmente */
    entr = scroll->prima_entry;
    while(entr){                /* invio le entries */
        if(entr->tipo != tipo || entr->ricevuta == 1){
            entr = entr->pros;
            continue;
        }
        var = htonl(entr->quantita);
        send(sd,&var,sizeof(uint32_t),0);
        entr = entr->pros;
    }
}

/* Trasferisce le entries locali al peer vicino al momento della disconnessione */
void trasferisci(struct registro* primo,int sd){
    char stringa[20] = "LEAVING";
    uint32_t size = strlen(stringa) +1;
    struct entry* scroll;

    if(!primo) return;

    size = htonl(size);
    send(sd, &size, sizeof(uint32_t), 0);
    size = ntohl(size);
    send(sd, stringa, size, 0); /* invio messaggio "LEAVING" */

    while(primo){
        scroll = primo->prima_entry;
        size = 0;
        while(scroll){  /* invio solo le entries salvate originariamente su questo peer */
            if(!scroll->ricevuta)
                size++;
            scroll = scroll->pros;
        }

        size = htonl(size);
        send(sd, &size, sizeof(uint32_t), 0);   /*invio il numero di entries nel registro corrente */
        
        scroll = primo->prima_entry;
        while(scroll){
            if(!scroll->ricevuta){
                sprintf(stringa,"%d %c",scroll->quantita,scroll->tipo);
                size = htonl(strlen(stringa) + 1);
                send(sd, &size, sizeof(uint32_t), 0);   
                size = ntohl(size);
                send(sd, stringa, size, 0); /* invio la entry in formato "<quantità> <tipo>" */
            }

            scroll = scroll->pros;
        }
        primo = primo->pros;
    }
    return;
}
/* Inizializza 4 registri con entries a scopo dimostrativo (scopo dimostrativo)*/
void inizializzazione(struct registro** primo, int porta,struct tm* data){
    char stringa[40];
    int i,y;
    FILE* fd;
    struct registro* scroll;
    char tipo;
    int quanti;

    sprintf(stringa, "inizializzazione/%d.txt", porta);

    fd = fopen(stringa,"r");
    for(i = 0; i < 4; i++){
        scroll = malloc(sizeof(struct registro));
        nuovo_registro(scroll);
        data->tm_mday --;
        mktime(data);
        dataStringa(scroll->data_reg,data);
        scroll->pros = *primo;
        *primo = scroll;
    }
    for(i = 0; i < 4; i++){
        for(y = 0; y < 5; y++){
            fgets(stringa, 6, fd);
            sscanf(stringa,"%d %c",&quanti,&tipo);
            inserisciEntry(&scroll,tipo,quanti,0);
        }
        scroll = scroll->pros;
    }
}

int main(int n_args, char** args){
    int d = 1;      /* usato per il comando "next" */
    int porta;      /* porta del socket TCP passata come parametro iniziale */
    int ret;
    int i;
    int listener;   /* socket TCP su cui si ricevono richieste di connessione dai peer vicini */
    int sd_UDP;     /* socket UDP per la comunicazione con il DS */
    int sd_TCP[2];     /* socket di comunicazione con i peer VICINI */
    char comando[10];
    int sd_PEER;    /* socket di comunicazione con i peer NON VICINI (quelli che richiedono i dati) */

    /* parametri del comando START */
    char indirizzo_ds[20];
    int porta_ds;       

    char buffer[BUF_SIZE];  /*buffer destinato allo STDIN */
    char msg[MSG_SIZE]; /* buffer dei messaggi destinati al DS */
    uint16_t porta_vicino; /* porta del vicino da contattare, ricevuto dal DS con il messaggio "NGB" */
    socklen_t len;
    uint32_t dim_msg_PEER; /* dimensione del messaggio ricevuto da un peer */
    char msg_PEER[500];         /* messaggio ricevuto dal peer */

    struct sockaddr_in indirizzo;   /* Peer */
    struct sockaddr_in ds;          /*DS */
    struct sockaddr_in vicino;      /*vicino */

    /*variabili per il I/O multiplexing */
    fd_set MASTER,READY;
    int max_fd;
    struct timeval* timeout = NULL;

    /* gestione delle entry */
    char tipo;                          /* tipo della entry inserita(ADD) o del dato aggregato richiesto(GET) */
    int quanti;                         /* quantità della entry inserita */                 
    struct registro* primo_reg = NULL;  /* registro iniziale */
    struct registro* corr_reg = NULL;   /* registro aperto */
    time_t now;
    struct tm* data;                    /* data da assegnare al registro */

    /* comando GET */
    char aggr[4];                       /* tipo di dato aggregato richiesto (GET), può essere TOT o VAR */
    char periodo1[DATA_LEN];            /* periodo iniziale (GET) */
    char periodo2[DATA_LEN];            /* periodo finale (GET) */
    struct registro* scroll;


    /* controllo il parametro <porta> iniziale */
    if(n_args != 2){
        printf("Errore: deve essere inserito un numero di porta (./peer <porta>)\n");
        exit(0);
    }
    ret = sscanf(args[1],"%d",&porta);
    if(!ret){
        printf("Non è stato inserito un numero di porta (./peer <porta>)\n");
        exit(0);
    }
    /* Socket TCP */
    listener = socket(AF_INET, SOCK_STREAM,0);

    memset(&indirizzo,0,sizeof(indirizzo));
    memset(&vicino,0,sizeof(vicino));
    memset(&ds,0,sizeof(ds));
    indirizzo.sin_port = htons(porta);
    indirizzo.sin_family = AF_INET;
    inet_pton(AF_INET, IP, &indirizzo.sin_addr);
    ret = bind(listener, (struct sockaddr*)&indirizzo, sizeof(indirizzo));
    if( ret == -1){
        perror("Errore: ");
        exit(0);
    }
    ret = 1;
    while(ret)
        ret = listen(listener, 5);

    /* Socket UDP */
    sd_UDP = socket(AF_INET, SOCK_DGRAM,0);
    len = sizeof(ds);
    
    FD_ZERO(&MASTER);
    FD_ZERO(&READY);
    FD_SET(STDIN_FILENO,&MASTER);
    FD_SET(sd_UDP,&MASTER);
    FD_SET(listener,&MASTER);

    max_fd = sd_UDP;
    if(listener > max_fd)   max_fd = listener;
    sd_TCP[0] = sd_TCP[1] = -1; 

    /* apro il primo registro, lo inizializzo e gli assegno la data corrente */
    primo_reg = malloc(sizeof(struct registro));
    corr_reg = primo_reg;
    nuovo_registro(corr_reg);
    time(&now);
    data = localtime(&now);
    if(data->tm_hour >= 18){
        d = 2;
        data->tm_mday +=1;
        mktime(data);
    }
    dataStringa(corr_reg->data_reg,data);
    help();

    inizializzazione(&primo_reg,porta,data);    /* riempe 4 registri con 5 entries lette dal file (scopo dimostrativo) */
    
    while(1){
        time(&now);
        data = localtime(&now);
        aggiornaRegistro(&corr_reg,data);   /* apro un nuovo registro se necessario */
        READY = MASTER;
        ret = select(max_fd +1, &READY, NULL, NULL, timeout);
        /* timeout, rimando la richiesta al ds */
        if(!ret){
            printf("TIMEOUT: non ho ricevuto risposta dal DS, provo a ricontattarlo\n");
            strcpy(msg,"NEW_PEER");
            ret = sendto(sd_UDP,msg,strlen(msg) +1, 0, (struct sockaddr*)&ds,sizeof(ds));
            if(ret == -1){
                perror("Errore:");
            }
            porta = htons(porta);
            sendto(sd_UDP,&porta, sizeof(uint16_t), 0,(struct sockaddr*)&ds,sizeof(ds));
            porta = ntohs(porta);   /* lo ripristino perché può servirmi successivamente */
            timeout->tv_sec = 4;
            timeout->tv_usec = 0;
        }
        for(i = 0; i <= max_fd; i++){
            if(!FD_ISSET(i,&READY))
                continue;
            if(i == STDIN_FILENO){
                fgets(buffer,BUF_SIZE,stdin);
                sscanf(buffer,"%s",comando);
                /* COMANDO ESC, invio il messaggio "RMV_PEER" al ds */
                if(!strcmp(comando,"esc")){
                    /* trasferisco le entries ai peer vicini */
                    printf("Trasferisco le entries ai peer vicini\n");
                    
                    if(FD_ISSET(sd_TCP[1],&MASTER)){
                        trasferisci(primo_reg,sd_TCP[1]);
                        close(sd_TCP[1]);
                    }
                    else if(FD_ISSET(sd_TCP[0],&MASTER))
                        trasferisci(primo_reg,sd_TCP[0]);

                    strcpy(msg,"RMV_PEER");
                    sendto(sd_UDP,msg,strlen(msg)+1,0,(struct sockaddr*)&ds,sizeof(ds));
                    printf("INVIATO RMV_PEER: disconnessione dalla rete\n");
                    close(sd_UDP);
                    close(listener);
                   
                    exit(0);
                }
                /*COMANDO HELP */
                else if(!strcmp(comando,"help"))
                    help();

                /*COMANDO START */
                else if(!strcmp(comando,"start")){
                    sscanf(buffer,"%s %s %d",comando,indirizzo_ds,&porta_ds);
                    inet_pton(AF_INET, indirizzo_ds,&ds.sin_addr);
                    ds.sin_family = AF_INET;
                    printf("%s %d\n",indirizzo_ds,porta_ds);
                    ds.sin_port = htons(porta_ds);
                    strcpy(msg,"NEW_PEER");
                    /* invio messaggio "NEW_PEER" */
                    ret = sendto(sd_UDP,msg,strlen(msg)+1, 0, (struct sockaddr*)&ds,sizeof(ds));
                    if(ret == -1){
                        perror("Errore:");
                        continue;
                    }
                    porta = htons(porta);
                    /* invio il numero di porta */
                    sendto(sd_UDP,&porta, sizeof(uint16_t), 0,(struct sockaddr*)&ds,sizeof(ds));
                    printf("INVIATO NEW_PEER: tentativo di connessione con il DS\n");
                    porta = ntohs(porta);   /* ripristinato per usarlo successivamente */
                    timeout = malloc(sizeof(struct timeval));
                    timeout->tv_sec = 4;
                    timeout->tv_usec = 0;
                }
                /*COMANDO ADD */
                else if(!strcmp(comando,"add")){
                    sscanf(buffer,"%s %c %d",comando,&tipo,&quanti);
                    ret = inserisciEntry(&corr_reg,tipo,quanti,0);
                    /* notifico il DS dell' inserimento della entry */
                    if(ret){
                        if(tipo == 'N') 
                            strcpy(msg,"INSERT N");
                        else    
                            strcpy(msg,"INSERT T");
                        sendto(sd_UDP,msg,strlen(msg)+1,0,(struct sockaddr*)&ds,sizeof(ds));
                        printf("Entry registrata nel peer e notificata al DS\n");
                    }
                    else
                        printf("Formato della entry non valido\n");
                }
                /* COMANDO GET */
                else if(!strcmp(comando,"get")){
                    sscanf(buffer,"%s %s %c %s %s",comando,aggr,&tipo,periodo1,periodo2);
                    ret = get(primo_reg,corr_reg,aggr,tipo,periodo1,periodo2,sd_UDP,&ds,sd_TCP,&MASTER,porta);
                    if(!ret)
                        printf("Parametri errati\n");
                }
                /* ----------------------COMANDO "next": scopo dimostrativo (simula l'apertura di un nuovo reg)--------------- */
                else if(!strcmp(comando,"next")){
                    data->tm_mday += d;
                    d++;
                    mktime(data);
                    corr_reg->pros = malloc(sizeof(struct registro));
                    corr_reg = corr_reg->pros;
                    nuovo_registro(corr_reg);
                    dataStringa(corr_reg->data_reg,data);
                    scroll = primo_reg;
                }
                /*--------------------------------------------------------------------------------------------------------------*/
                
            }
            /* COMUNICAZIONE CON IL DS */
            if(i == sd_UDP){
                recvfrom(sd_UDP,msg,DS_MSG_SIZE,0,(struct sockaddr*)&ds,&len);
                
                /* messaggio "NGB" */
                if(!strcmp(msg,"NGB")){ /* nuovo peer da contattare */
                    recvfrom(sd_UDP,&porta_vicino,sizeof(uint16_t),0,(struct sockaddr*)&ds,&len);
                    porta_vicino = ntohs(porta_vicino);
                    if(FD_ISSET(sd_TCP[1],&MASTER)){    /* chiudo il vecchio socket (quello comunicante con il precedente vicino) */
                        close(sd_TCP[1]);
                        FD_CLR(sd_TCP[1],&MASTER);
                    }
                    if(!porta_vicino)
                        continue;

                    printf("RICEVUTO NGB: il DS mi ha assegnato un nuovo vicino con porta %d\n",porta_vicino);
                    vicino.sin_family = AF_INET;
                    vicino.sin_port = htons(porta_vicino);
                    inet_pton(AF_INET,IP,&vicino.sin_addr);

                    sd_TCP[1] = socket(AF_INET,SOCK_STREAM,0);
                    do{
                        ret = connect(sd_TCP[1],(struct sockaddr*)&vicino,sizeof(vicino));
                    } while(ret == -1);
            
                    FD_SET(sd_TCP[1],&MASTER);
                    if(sd_TCP[1] > max_fd) max_fd = sd_TCP[1];

                    /* invio il messaggio di presentazione "NEIGHBOUR" al nuovo vicino */
                    strcpy(msg_PEER,"NEIGHBOUR");
                    /* Invio dimensione del messaggio e messaggio NEIGHBOUR */
                    dim_msg_PEER = strlen(msg_PEER) + 1;
                    dim_msg_PEER = htonl(dim_msg_PEER);
                    send(sd_TCP[1],&dim_msg_PEER,sizeof(uint32_t),0);
                    dim_msg_PEER = ntohl(dim_msg_PEER);
                    send(sd_TCP[1],msg_PEER,dim_msg_PEER,0);
                    printf("INVIATO NEIGHBOUR\n");
                    break;     
                }
                else if(!strcmp(msg,"EXT")){    /*disconnessione */
                    printf("RICEVUTO ESC: il DS si è disconnesso\n");
                    exit(0);
                }
                else if(!strcmp(msg,"ACK")){
                    printf("RICEVUTO ACK: registrazione alla rete effettuata\n");
                    free(timeout);
                    timeout = NULL;
                }
                
            }
            /* Nuove richieste di comunicazione da parte dei peer */
            if(i == listener){
                /* creo il socket di comunicazione con il peer */
                sd_PEER = accept(listener,(struct sockaddr*)&vicino,&len);

                recv(sd_PEER,&dim_msg_PEER,sizeof(uint32_t),0);    
                dim_msg_PEER = ntohl(dim_msg_PEER);
                recv(sd_PEER,msg_PEER,dim_msg_PEER,0);  /* ricevo il messaggio "INTRODUTTIVO", per capire che tipo di peer sia (vicino o richiedente) */

                if(!strcmp(msg_PEER,"NEIGHBOUR")){  /* Il peer è un nuovo vicino */
                    printf("RICEVUTO NEIGHBOUR: aggiunto il nuovo vicino\n");
                    FD_CLR(sd_TCP[0],&MASTER);   
    
                    sd_TCP[0] = sd_PEER;    /* inserisco il socket fra quelli di comunicazione dei vicini */
                    FD_SET(sd_TCP[0],&MASTER);
                    if(sd_TCP[0] > max_fd)  max_fd = sd_TCP[0];
                    sd_PEER = -1;
                }
                else if(!strcmp(msg_PEER,"REQUESTER")){ /* Il peer è un richiedente (di entries) */
                    printf("RICEVUTO REQUESTER: connesso con il peer che richiede i dati\n");
                    FD_SET(sd_PEER,&MASTER);
                    if(sd_PEER > max_fd)    max_fd = sd_PEER;
                }
            }
            /* Messaggi dai vicini */
            if(i == sd_TCP[1] || i == sd_TCP[0]){
                ret = recv(i,&dim_msg_PEER,sizeof(uint32_t),0); /* ricevo la dimensione del messaggio */
                if(!ret){   /* chiuso il socket del vicino (si è disconnesso o è arrivato un nuovo vicino) */
                    FD_CLR(i,&MASTER);
                    close(i);
                    if(sd_TCP[0] == i)
                        sd_TCP[0] = -1;
                    else
                        sd_TCP[1] = -1;
                    continue;
                }
                dim_msg_PEER = ntohl(dim_msg_PEER);   
                recv(i,msg_PEER,dim_msg_PEER,0);    /* ricevo il messaggio */
                sscanf(msg_PEER,"%s",buffer);
                /* Messaggio REQ_DATA <data> <tipo> */
                if(!strcmp(buffer,"REQ_DATA")){
                    sscanf(msg_PEER,"%s %s %c",buffer,periodo1,&tipo);
                    scroll = trovaRegistro(primo_reg,periodo1);
                    if(!scroll){
                        strcpy(msg_PEER,"REPLY_DATA -1");
                    }
                    else{   /* controllo se ho calcolato il dato */
                        if(tipo == 'N' && scroll->aggr_N.finale_TOT)
                            sprintf(msg_PEER,"REPLY_DATA %d",scroll->aggr_N.TOT);
                        else if(tipo == 'T' && scroll->aggr_T.finale_TOT)
                            sprintf(msg_PEER,"REPLY_DATA %d",scroll->aggr_T.TOT);
                        else
                            strcpy(msg_PEER,"REPLY_DATA -1");
                    }
                    dim_msg_PEER = strlen(msg_PEER) + 1;
                    dim_msg_PEER = htonl(dim_msg_PEER);
                    send(i,&dim_msg_PEER,sizeof(uint32_t),0);
                    dim_msg_PEER = ntohl(dim_msg_PEER);
                    send(i,msg_PEER,dim_msg_PEER,0);    /* messaggio di risposta */
                    printf("INVIATO REPLY_DATA\n");
                }
                /* Messaggio: FLOOD_FOR_ENTRIES <data> <tipo>*/
                else if(!strcmp(buffer,"FLOOD_FOR_ENTRIES")){
                    printf("%s\n",msg_PEER);
                    sscanf(msg_PEER,"%s %s %c",buffer,periodo1,&tipo);
                    scroll = trovaRegistro(primo_reg,periodo1);
                    if(scroll){
                        if((tipo == 'N' && scroll->tot_N) || (tipo == 'T' && scroll->tot_T)){ /* controllo di avere delle entries salvate localmente */
                            sprintf(buffer," %d",porta);
                            strcat(msg_PEER,buffer);    /* aggiungo al messaggio ricevuto il numero di porta di questo peer */
                        }
                    }
                    /* Invio il messaggio (uguale o aggiungendo il mio numero di porta) al vicino "alla sinistra" 
                        (quello con cui comunico usando sd_TCP[1]) */
                    dim_msg_PEER = strlen(msg_PEER) + 1;
                    dim_msg_PEER = htonl(dim_msg_PEER);
                    /* caso particolare: rete con 2 peer-> il 2° peer deve inoltrare usando sd_TCP[0] */
                    if(!FD_ISSET(sd_TCP[1],&MASTER)){   
                        send(sd_TCP[0],&dim_msg_PEER,sizeof(uint32_t),0);
                        dim_msg_PEER = ntohl(dim_msg_PEER);
                        send(sd_TCP[0],msg_PEER,dim_msg_PEER,0);
                    }
                    else{
                        send(sd_TCP[1],&dim_msg_PEER,sizeof(uint32_t),0);
                        dim_msg_PEER = ntohl(dim_msg_PEER);
                        send(sd_TCP[1],msg_PEER,dim_msg_PEER,0);
                    }
                }
                else if(!strcmp(buffer,"REQ_ENTRIES")){
                    printf("RICEVUTO REQ_ENTRIES\n");
                    sscanf(msg_PEER,"%s %s %c",buffer,periodo1,&tipo);  /* REQ_ENTRIES <data> <tipo> */
                    scroll = trovaRegistro(primo_reg,periodo1);
                    REQ_ENTRIES(msg_PEER,tipo,i,scroll);
                }
                else if(!strcmp(buffer,"LEAVING")){
                    printf("RICEVUTO LEAVING\n");
                    scroll = primo_reg;
                    while(scroll){
                        recv(i, &dim_msg_PEER, sizeof(uint32_t), 0);    /* ricevo il numero di entries per un registro */
                        dim_msg_PEER = ntohl(dim_msg_PEER);
                        if(!dim_msg_PEER){
                            scroll = scroll->pros;
                            continue;
                        }
                        while(dim_msg_PEER){
                            uint32_t size;
                            recv(i, &size, sizeof(uint32_t), 0);
                            size = ntohl(size);
                            recv(i, buffer, size, 0);   /* ricevo la stringa di tipo "<quantità> <tipo>" */
                            sscanf(buffer, "%d %c", &quanti, &tipo);
                            inserisciEntry(&scroll, tipo, quanti,0);

                            dim_msg_PEER--;
                        }
                        scroll = scroll->pros;
                    }
                }
            }
            /* PEER RICHIEDENTI ENTRIES */
            if(i == sd_PEER){
                recv(i,&dim_msg_PEER,sizeof(uint32_t),0);
                dim_msg_PEER = ntohl(dim_msg_PEER);
                recv(i,msg_PEER,dim_msg_PEER,0);
                printf("RICEVUTO REQ_ENTRIES\n");
                sscanf(msg_PEER,"%s %s %c",buffer,periodo1,&tipo);  /* REQ_ENTRIES <data> <tipo> */
                scroll = trovaRegistro(primo_reg,periodo1);
                REQ_ENTRIES(msg_PEER,tipo,i,scroll);

                FD_CLR(sd_PEER,&MASTER);    /* chiudo il socket */
                close(sd_PEER);
                sd_PEER = -1;
                
            }
        }
    }
    return 0;
}