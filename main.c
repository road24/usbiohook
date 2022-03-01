/*
 * Necesitamos una forma mas portable de usar el piuio entre os
 * SM5 ya tiene por defecto un driver que usa modulos de kernel
 * la cosa es que compilar el modulo no es algo que me gusta hacer
 * y menos si pienso usar un os ya preparado (HIUOS , AMOS)
 * asi que la solucion es engañar al codigo que usa el modulo de kernel
 * y usar libusb
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <usb.h>
#include <sys/types.h>
#include <sys/stat.h>
//#include <fcntl.h>

// un macro para ayudar
#define SET_REQUEST(reg,val) reg = (reg & 0xFC) | ( val & 0x03 )

// definiciones
#define PIUIO_VID 		0x0547
#define PIUIO_PID 		0x1002
#define PIUIO_INT		0x00
#define PIUIO_REQ   	0xAE

#ifdef WITH_DEBUG
#define DEBUG(fmt,vargs...) printf("IOHOOK: " fmt,##vargs)
#else
#define DEBUG(fmt,vargs...) do{}while(0)
#endif // WITH_DEBUG
/*
 * Estructura para el manejo del piuio
 *
 */
struct piuio_Context
{
     struct usb_device     *device;
     struct usb_dev_handle *handler;
     char inputs[32];
     char lights[8];
};



struct piuio_Context PIUIO;
// El nombre que buscara sm5
char piuiodevpath[] = "/dev/piuio0";

// El fd que le asigare al piuio, muy alto para que no se sobrelape
const int piuiofd = 32768;

 // Las funciones reales a las que hare hook
int     (*real_open)(const char *, int);
ssize_t (*real_read)(int,void*,size_t);
ssize_t (*real_write)(int , const void *, size_t );
int     (*real_close)(int);
int     (*real_fstat)(int , struct stat *);
int     (*real___fxstat)(int , int , struct stat *);
//TODO
// fstat
//
// S_ISCHR( st.st_mode )
// constructor
void __attribute__((constructor)) initialize(void)
{
    DEBUG("Hooking functions\n");
    //
    real_open       = dlsym(RTLD_NEXT, "open");
    real_read       = dlsym(RTLD_NEXT, "read");
    real_write      = dlsym(RTLD_NEXT, "write");
    real_close      = dlsym(RTLD_NEXT, "close");
    real_fstat      = dlsym(RTLD_NEXT, "fstat");
    real___fxstat   = dlsym(RTLD_NEXT, "__fxstat");
}

struct usb_device *findPIUIO()
{

    struct usb_bus      *bus = NULL;
    struct usb_device   *dev = NULL;
    // busco en cada bus
    for(bus = usb_get_busses(); bus; bus = bus->next)
    {
        for(dev = bus->devices; dev; dev = dev->next)
        {
            if(dev->descriptor.idVendor == 0x0547 && dev->descriptor.idProduct == 0x1002 )
            {
                return dev;
            }
        }
    }
    return NULL;
}


int connectPIUIO()
{
    usb_init();
    if (usb_find_busses() <= 0)
    {
        DEBUG("usb_find_busses failded\n");
        return -1;
    }

    if (usb_find_devices() <= 0)
    {
        DEBUG("usb_find_devices failded\n");
        return -1;
    }

    PIUIO.device = findPIUIO();

    if( !PIUIO.device )
    {
        DEBUG("PIUIO Not Found\n");
        return -1;
    }
    PIUIO.handler = NULL;
    PIUIO.handler = usb_open(PIUIO.device);
    if( !PIUIO.handler )
    {
        DEBUG("usb_open error\n");
        return -1;
    }

    if (usb_set_configuration(PIUIO.handler, 1) < 0)
    {
        DEBUG("usb_set_configuration error\n");
        usb_close(PIUIO.handler);
        return -1;
    }
    // TODO : Usar interface guardada en el descriptor ???
    usb_detach_kernel_driver_np(PIUIO.handler,0);

    if (usb_claim_interface(PIUIO.handler,0) < 0)
    {
        DEBUG("usb_claim_interface error\n");
        usb_close(PIUIO.handler);
        return -1;
    }
    // Si llegamos hasta aqui significa que podemos usarlo ...
    return 0;
}

int open( const char *path, int mode)
{
    // sanity check
    if( !real_open )
    {
        real_open = dlsym(RTLD_NEXT,"fopen");
    }
    // si es el piuio el que se quiere abrir
    if( !memcmp(path,"/dev/piuio",10) )
    {
            DEBUG("Intentado conectar con PIUIO ");
            // Intento conectarme con el PIUIO
            if( !connectPIUIO() )
            {
                DEBUG("OK\n");
                return piuiofd;
            }
            else
            {
                DEBUG("ERROR\n");
                return -1;
            }
    }
    // sino paso el control a open
    return real_open(path,mode);
}


int close(int fd)
{
    if( !real_close )
    {
        real_close = dlsym(RTLD_NEXT,"close");
    }
    if( fd == piuiofd )
    {
        if( PIUIO.handler )
        {
            usb_close(PIUIO.handler);
        }
        return 0;
    }
    return real_close(fd);
}



ssize_t read(int fd,void *data,size_t bytes)
{
    if( !real_read )
    {
        real_read = dlsym(RTLD_NEXT,"read");
    }
    if( fd != piuiofd )
    {
        return real_read(fd,data,bytes);
    }

    if( bytes != 32 )
    {
        DEBUG("Error, expected 32 bytes to read\n");
        return 0;
    }

    // leo los datos de los sensores
    char i;
    for( i=0;i<4;i++)
    {
        // Le tengo que decir que sensor quiero
        // estoy indicando bien la salida ??
        SET_REQUEST(PIUIO.lights[0],i);
        SET_REQUEST(PIUIO.lights[2],i);
        usb_control_msg(PIUIO.handler, 0x40, PIUIO_REQ, 0, 0, PIUIO.lights, 8, 10000);
        // Ahora leo el sensor que pedi
        usb_control_msg(PIUIO.handler, 0xC0, 0xAE, 0, 0, &PIUIO.inputs[i*8], 8, 10000);
    }
    //Ahora copio las lecturas
    DEBUG("UPDATING INPUT\n");
    memcpy(data,PIUIO.inputs,32);
    return 32;
}

ssize_t write(int fd, const void * data, size_t bytes)
{
    if( !real_write )
    {
        real_write = dlsym(RTLD_NEXT,"write");
    }
    if( fd != piuiofd )
    {
        return real_write(fd,data,bytes);
    }

    if( bytes != 8 )
    {
        DEBUG("Error, expected 8 bytes to write");
        return 0;
    }
    // Aqui es mas simple solo actualizo las luces
    // ya que el que se encarga de enviar los datos es read
    DEBUG("UPDATING OUTPUT\n");
    memcpy(PIUIO.lights,data,8);
    return 8;
}
/** Esta no la usa pero la dejo por si las dudas */
int fstat (int __fd, struct stat *__buf)
{
    if( !real_fstat )
    {
        real_fstat = dlsym(RTLD_NEXT,"fstat");
    }
    if( __fd != piuiofd )
        return real_fstat(__fd,__buf);
    DEBUG("STATING FAKE PIUIO\n");
    memset(__buf,0x00,sizeof(struct stat));
    __buf->st_mode = __S_IFCHR;
    return 0;
}

/** Esta es la que usa al final sm5 al menos en mi pc */
int __fxstat(int ver, int __fd, struct stat *__buf)
{
    if( !real___fxstat )
    {
        real___fxstat = dlsym(RTLD_NEXT, "__fxstat");
    }
    if( __fd != piuiofd )
        return real___fxstat(ver,__fd,__buf);
    DEBUG("STATING FAKE PIUIO\n");
    memset(__buf,0x00,sizeof(struct stat));
    __buf->st_mode = __S_IFCHR;
    return 0;
}
