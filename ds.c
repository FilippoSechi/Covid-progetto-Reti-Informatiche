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

#define BUF_SIZE 100
#define PEER_MSG_SIZE 9  /* lunghezza dei messaggi ricevuti dai peer */
#define DATA_LEN 11     /* lunghezza dalla data nel formato yyyy:mm:dd */
#define REPLY_SIZE 4    /* lunghezza messaggi inviati ai peer */

/* struttura per la memorizzazione dei peer registrati */
struct peer{
    struct sockaddr_in indirizzo;   /* socket UDP del peer con cui contattarlo */
    struct peer* prec;
    struct peer* pros;
    uint16_t porta;                /* porta del socket TCP del peer, usato nelle comunicazioni peer-to-peer */
};

/* Struttura per la memorizzazione del numero di entry registrate globalmente (giorno per giorno) */
struct TOT_entry{
    uint32_t tamponi;  /* totale tamponi giornalieri */
    uint32_t nuovi_casi;   /* totale nuovi casi giornalieri */
    struct TOT_entry* pros;
    char data[DATA_LEN];
};

/* COMANDO HELP--> mostra tutti i possibili comandi */
void help(){
    printf("**************DS COVID**********\n");
    printf("Digita un comando:\n");
    printf("1)help --> mostra i dettagli dei comandi\n");
    printf("2)showpeers --> mostra un elenco dei peer connessi\n");
    printf("3)showneighbour <peer> --> mostra i neighbor di un peer\n");
    printf("4) esc --> chiude il DS\n");
    printf("---\n");
    return ;
}
/* COMANDO SHOWPEERS--> mostra le porte di tutti i peer connessi */
void showpeers(struct peer* testa){
    struct peer* scroll = testa;
    if(!testa)
        printf("Non ci sono peer connessi\n");

    while(scroll){
        printf("Peer: %d\n",scroll->porta);
        scroll = scroll->pros;
        if(scroll == testa)
            break;
    }
    printf("---\n");
    return;
}
/* COMANDO ESC--> invia un messaggio "EXT" a tutti i peer e termina il ds */
void esc(struct peer** testa,int sd){
    struct peer* scroll = *testa;
    char msg[4] = "EXT";
    if(!scroll) return;
    if((*testa)->prec)  /*spezzo l'anello per poter uscire */
        (*testa)->prec->pros = NULL;
    /* invio il messaggio di uscita "EXT" a tutti i peer */
    do{
        *testa = scroll;
        sendto(sd, msg, REPLY_SIZE, 0, (struct sockaddr*)&(scroll->indirizzo), sizeof(struct sockaddr_in));
        scroll = scroll->pros;
        free(*testa);
    }   while(scroll);

    return;
}
/* COMANDO SHOWNEIGHBOUR--> mostra i vicini di uno (o tutti) i peer */
void showneighbour(struct peer* testa, int porta){
    struct peer* scroll = testa;
    while(scroll){
        if(porta == -1 || scroll->porta == porta){
            if(!scroll->pros || !scroll->prec){
                printf("Peer %d --> [-][-]\n",scroll->porta);   /* se non ha vicini */
                return;
            }
            printf("Peer %d --> [%d][%d]\n",scroll->porta,scroll->prec->porta, scroll->pros->porta);
        }
        scroll = scroll->pros;
        if(scroll == testa) break;
    }
    printf("---\n");
    return;
}

/* COMUNICA AD UN PEER I SUOI VICINI
   I peer ricevono un messaggio "NGB" seguito da un messaggio contenente il numero di porta del vicino.
   Ogni peer deve occuparsi di contattare solo il peer->pros nella rete. Se un peer non ha più vicini riceve 0 come numero di porta*/
void aggiornaVicini( struct peer* ricevente, int sd){
    char msg[4] = "NGB";
    uint16_t porta;
    if(!ricevente->pros)    /* ultimo peer della rete */
        porta = 0;
    else
        porta = htons(ricevente->pros->porta);

    /* invio "NGB" e numero di porta del vicino */
    sendto(sd,msg,REPLY_SIZE,0,(struct sockaddr*)&(ricevente->indirizzo),sizeof(ricevente->indirizzo));
    sendto(sd,&porta,sizeof(uint16_t),0,(struct sockaddr*)&(ricevente->indirizzo),sizeof(ricevente->indirizzo));

    return;
}

/* AGGIUNGE UN NUOVO PEER NELLA RETE */
void aggiungiPeer(struct peer** testa,struct peer* temp, int sd){
    struct peer* scroll1;
    struct peer* scroll2;

    if(!*testa){     /* primo peer nella rete */
        temp->prec = temp->pros = *testa;
        *testa = temp;
        return;
    }

    if((*testa)->porta == temp->porta){  /* peer già inserito */
        free(temp);
        return;
    }

   if(!(*testa)->pros && !(*testa)->prec){     /* secondo peer della rete, creo l'anello */ 
        (*testa)->prec = (*testa)->pros = temp;
        temp->prec = temp->pros = *testa;
        if((*testa)->porta > temp->porta)
            *testa = temp;
        aggiornaVicini(*testa,sd);
        return;
    }

    if((*testa)->porta > temp->porta){ /* inserimento in testa */
        temp->pros = *testa;
        temp->prec = (*testa)->prec;
        (*testa)->prec = temp;
        temp->prec->pros = temp;
        *testa = temp;
        aggiornaVicini(temp->prec,sd);
        aggiornaVicini(temp,sd);
        return;
    }

    scroll1 = scroll2 = (*testa)->pros;
    while(scroll1->porta < temp->porta){    
        if(scroll1 == *testa)
            break;
        scroll2 = scroll1;
        scroll1 = scroll1->pros;
    }

    if(scroll1->porta == temp->porta){ /*peer già inserito*/
        free(temp);
        return;
    }

    if(scroll1 == scroll2){
        scroll1->prec->pros = temp;
        temp->prec = scroll1->prec;
        temp->pros = scroll1;
        scroll1->prec = temp;
        if(scroll1->prec == (*testa)->pros){
            scroll1->pros = (*testa);
            (*testa)->prec = scroll1;
        }
    }

    else {
        scroll1->prec = temp;
        scroll2->pros = temp;
        temp->prec = scroll2;
        temp->pros = scroll1;
    }
    if((*testa) == temp->prec && (*testa)->prec == temp->pros)
        aggiornaVicini(temp->pros,sd);

    aggiornaVicini(temp->prec,sd);
    aggiornaVicini(temp,sd);
    return; 

}  
/* RIMUOVE PEER DALLA RETE */
void rimuoviPeer(struct peer** testa,uint16_t porta, int sd){

    struct peer* scroll = *testa;
    if(!(*testa)) return;
    if(!scroll->prec && !scroll->pros){     /* un solo peer nella rete */
        if(ntohs(scroll->indirizzo.sin_port) != porta)
            return;
        else
            *testa = NULL;
            free(scroll);
            return;
    }

    if(ntohs((*testa)->indirizzo.sin_port) == porta){ /*rimozione in testa */
        if((*testa)->pros == (*testa)->prec){       /*solo due peer */
            *testa = (*testa)->prec;
            (*testa)->prec = (*testa)->pros = NULL;
            aggiornaVicini(*testa,sd);
            free(scroll);
            return;
        }
        *testa = scroll->pros;
        scroll->prec->pros = *testa;
        (*testa)->prec = scroll->prec;
        aggiornaVicini(scroll->prec, sd);
        free(scroll);
        return;
    }
    scroll = scroll->pros;
    while(scroll != *testa){
        if(ntohs(scroll->indirizzo.sin_port) == porta)
            break;
        scroll = scroll->pros;
    }

    if(scroll == *testa) return;    /* peer non trovato */
    
    if(scroll->prec == scroll->pros){ /* due peer nella rete, rimozione di quello non in testa */
        (*testa)->pros = (*testa)->prec = NULL;
    }
    else{
        scroll->prec->pros = scroll->pros;
        scroll->pros->prec = scroll->prec;
        if(scroll->prec->pros != (*testa))
            aggiornaVicini(scroll->prec,sd);
    }
    free(scroll);
    return;
}

/* trasforma una data in una stringa */
void dataStringa(char* stringa, struct tm* data){
    sprintf(stringa,"%04d:%02d:%02d",data->tm_year+1900,data->tm_mon+1,data->tm_mday);
    return;
}

/* inizializza una nuova struttura TOT_entry */
void nuova_TOT_entry(struct TOT_entry* reg,struct tm* data){
    reg->pros = NULL;
    reg->tamponi = reg->nuovi_casi = 0;
    dataStringa(reg->data,data);
    return;
}

/* apro un nuovo TOT_entry (se necessario) */
void aggiorna_TOT_entry(struct TOT_entry** reg, struct tm* time){
    char stringa[DATA_LEN];
    int res;

    /* controllo l'orario */
    dataStringa(stringa,time);
    res = strcmp(stringa,(*reg)->data);
    if(res < 0) return; /* registro del giorno successivo già aperto */
    if(!res && time->tm_hour < 18)  return; /* non sono ancora le 18 */


    if(!res && time->tm_hour >= 18){
        (*reg)->pros = malloc(sizeof(struct TOT_entry));
        *reg = (*reg)->pros;   
        time->tm_mday += 1;
        mktime(time);
        nuova_TOT_entry(*reg,time);
        return;
    }
}

/* restituisce il totale di entries di tipo <tipo> nella data <data> 
    Il risultato è gia nel formato network */
uint32_t trovaTOT(char* data,struct TOT_entry* reg,char tipo){
    while(strcmp(reg->data,data) && reg->pros){
        reg = reg->pros;
    }

    if(tipo == 'N')
        return htonl(reg->nuovi_casi);
    else
        return htonl(reg->tamponi);
}

void inizializzazione (struct TOT_entry** primo,struct tm* data){
    struct TOT_entry* scroll;
    int i;

    for(i = 0; i < 4; i++){
        scroll = malloc(sizeof(struct TOT_entry));
        data->tm_mday --;
        mktime(data);
        nuova_TOT_entry(scroll,data);
        scroll->tamponi = 3*5;
        scroll->nuovi_casi = 2*5;
        scroll->pros = *primo;
        *primo = scroll;
    }
}

int main(int n_args, char** args){
    int d = 1;
    int ret;   
    int i;  /* usato nei cicli for */
    int porta;  /* porta del ds */
    int sd;     /* descrittore del socket UDP*/
    socklen_t len;
    char comando[20];   /* buffer per il comando ricevuto dall'utente */
    char buffer[BUF_SIZE];  /* buffer di memorizzazione dello stdin */
    char msg[PEER_MSG_SIZE];     /* buffer di memorizzazione dei messaggi del socket */
    int porta_SN;         /* parametro del comando showneighbour */
    struct peer* testa = NULL;     /* testa della rete di peer */
    struct peer* scroll;
    uint16_t porta_peer;           /* porta del socket TCP del peer */

    struct sockaddr_in ds, com_peer;    /* ds server e peer con cui comunico */
    struct peer* temp;                  /* usato come appoggio per allocare peer nella rete */

    /* variabili multiplexing I/O */
    int max_fd;
    fd_set MASTER,READY;

    /* variabili per la gestione delle entry nei peer */
    struct TOT_entry* primo_TOT_entry;
    struct TOT_entry* ultimo_TOT_entry;
    time_t now;
    struct tm* data;

    /* variabili messaggio REQUES N e REQUES T */
    char tipo;  /* tipo ricevuto con il messaggio REQUES */
    char stringa[PEER_MSG_SIZE]; /* variabile di appoggio usato dopo la ricezione del messaggio REQUES */
    char data_req[DATA_LEN];
    uint32_t contatore;    /* totale di entries giornaliere */

    /* inizializzazione del primo TOT_entry */
    time(&now);
    data = localtime(&now);
    primo_TOT_entry = malloc(sizeof(struct TOT_entry));
    ultimo_TOT_entry = primo_TOT_entry;
    if(data->tm_hour >= 18){
        d = 2;
        data->tm_mday += 1;
        mktime(data);
    }
    nuova_TOT_entry(ultimo_TOT_entry,data);

    /* controllo il parametro <porta> iniziale */
    if(n_args != 2){
        printf("Errore: deve essere inserito un numero di porta (./ds <porta>)\n");
        exit(0);
    }
    ret = sscanf(args[1],"%d",&porta);
    if(!ret){
        printf("Non è stato inserito un numero di porta (./ds <porta>)\n");
        exit(0);
    }

    help();
    sd = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&ds,0,sizeof(struct sockaddr_in));
    ds.sin_family = AF_INET;
    ds.sin_addr.s_addr = INADDR_ANY;
    ds.sin_port = htons(porta);
    ret = bind(sd,(struct sockaddr*)&ds, sizeof(ds));
    if(ret == -1){
        perror("Errore:");
        exit(0);
    }

    FD_ZERO(&MASTER);
    FD_ZERO(&READY);

    FD_SET(STDIN_FILENO, &MASTER);
    FD_SET(sd, &MASTER);

    max_fd = sd;
    len = sizeof(com_peer);
    inizializzazione(&primo_TOT_entry,data);

    while(1){
        READY = MASTER;
        /* controllo se è l'ora di aprire un nuovo TOT_entry */
        time(&now);
        data = localtime(&now);
        aggiorna_TOT_entry(&ultimo_TOT_entry,data);

        select(max_fd+1, &READY, NULL, NULL, NULL);
        for(i = 0; i <= max_fd; i++){
            if(!FD_ISSET(i,&READY))
                continue;

            if(i == STDIN_FILENO){   /* ricevuto un nuovo comando */
                fgets(buffer, BUF_SIZE, stdin);
                
                if(!strcmp(buffer, "help\n"))   /* comando help */
                    help();
                
                else if(!strcmp(buffer,"showpeers\n"))  /* comando showpeers */
                    showpeers(testa);
                
                else if(!strcmp(buffer, "esc\n")){   /* comando esc */
                    esc(&testa,sd);
                    close(sd);
                    exit(0);
                }
                
                /*------------------------COMANDO "next": scopo dimostrativo (simula l'apertura di un nuovo reg)----------------- */    
                else if(!strcmp(buffer,"next\n")){
                    data->tm_mday += d;
                    d++;
                    mktime(data);
                    ultimo_TOT_entry->pros = malloc(sizeof(struct TOT_entry));
                    ultimo_TOT_entry = ultimo_TOT_entry->pros;
                    nuova_TOT_entry(ultimo_TOT_entry,data);
                }
                /*-------------------------------------------------------------------------------------------------------------*/
                else{
                    sscanf(buffer,"%s %d",comando,&porta_SN);
                    if(!strcmp(comando,"showneighbour")){    /* comando showneighbour */
                        if(!strcmp(buffer,"showneighbour\n"))   /* senza parametro */
                         showneighbour(testa,-1); 
                        else{                                   /* con parametro */
                         showneighbour(testa,porta_SN);
                        } 
                    }
                    else{  
                    printf("Comando errato, digitare help per la lista dei comandi possibili\n"); 
                    }
                }
            }
            if(i == sd){    /*ricevuta richiesta da un peer */
                recvfrom(sd, msg, PEER_MSG_SIZE, 0,(struct sockaddr*)&com_peer, &len);

                /* invio l'ACK e inserisco il peer nella rete */
                if(!strcmp(msg, "NEW_PEER")){
                    /* ricevo il numero di porta con cui il peer vuole essere contattato dai peer */
                    recvfrom(sd, &porta_peer, sizeof(uint16_t),0, (struct sockaddr*)&com_peer,&len);  
                    strcpy(msg,"ACK");
                    /* invio l'ACK */
                    sendto(sd,msg,strlen(msg) + 1, 0, (struct sockaddr*)&com_peer, sizeof(com_peer));

                    /* aggiungo il peer alla rete */
                    porta_peer = ntohs(porta_peer);
                    temp = (struct peer*)malloc(sizeof(struct peer));
                    temp->indirizzo = com_peer;
                    temp->porta = porta_peer;
                    aggiungiPeer(&testa, temp, sd);
                }

                /* rimuovo il peer dalla rete */
                else if(!strcmp(msg, "RMV_PEER")){
                    porta_peer = ntohs(com_peer.sin_port);
                    rimuoviPeer(&testa,porta_peer, sd);
                }
                /* aggiunta di una entry di tipo NUOVO_CASO (N) */
                else if(!strcmp(msg, "INSERT N")){
                    ultimo_TOT_entry->nuovi_casi++;
                }
                /* aggiunta di una entry di tipo TAMPONE (T) */
                else if(!strcmp(msg, "INSERT T")){
                    ultimo_TOT_entry->tamponi++;
                }
                else if(!strcmp(msg,"REQUES N") || !strcmp(msg,"REQUES T")){
                    sscanf(msg,"%s %c",stringa,&tipo);  
                    recvfrom(sd,data_req,DATA_LEN,0,(struct sockaddr*)&com_peer,&len);  /* ricevo la data */
                    contatore = trovaTOT(data_req,primo_TOT_entry,tipo);
                    /* invio il totale delle entries richieste */
                    sendto(sd,&contatore,sizeof(uint32_t),0,(struct sockaddr*)&com_peer,sizeof(com_peer));
                }
                /* il peer vuole sapere i numeri di porta dei vicini */
                else if(!strcmp(msg,"MY_NEIGH")){
                    /* ricevo il numero di porta del vicino richiedente */
                    recvfrom(sd,&porta_peer,sizeof(uint16_t),0,(struct sockaddr*)&com_peer,&len); /* ricevo la porta del peer che vuole conoscere i suoi vicini */
                    porta_peer = ntohs(porta_peer);
                    scroll = testa;
                    /* invio i numeri di porta dei vicini */
                    while(scroll->porta != porta_peer)
                        scroll = scroll->pros;
                    if(scroll->prec && scroll->prec != scroll->pros)
                        porta_peer = htons(scroll->prec->porta);
                    else
                        porta_peer = 0;
                    sendto(sd,&porta_peer,sizeof(uint16_t),0,(struct sockaddr*)&com_peer,sizeof(com_peer));
                    if(scroll->pros)
                        porta_peer = htons(scroll->pros->porta);
                    else
                        porta_peer = 0;
                    sendto(sd,&porta_peer,sizeof(uint16_t),0,(struct sockaddr*)&com_peer,sizeof(com_peer));
                }
            }
        }
    }

    return 0;
}