#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/string.h>

MODULE_AUTHOR("DevTITANS <devtitans@icomp.ufam.edu.br>");
MODULE_DESCRIPTION("Driver de acesso ao SmartLamp (ESP32 com Chip Serial CP2102)");
MODULE_LICENSE("GPL");

#define MAX_RECV_LINE 100
#define VENDOR_ID   0x10C4   // Vendor ID do CP2102
#define PRODUCT_ID  0xEA60   // Product ID do CP2102

static struct usb_device *smartlamp_device;
static uint usb_in, usb_out;
static char *usb_in_buffer, *usb_out_buffer;
static int usb_max_size;


static struct usb_device *smartlamp_device;        // Referência para o dispositivo USB
static uint usb_in, usb_out;                       // Endereços das portas de entrada e saida da USB
static char *usb_in_buffer, *usb_out_buffer;       // Buffers de entrada e saída da USB
static int usb_max_size;                           // Tamanho máximo de uma mensagem USB

#define VENDOR_ID   SUBSTITUA_PELO_VENDORID /* Encontre o VendorID  do smartlamp */
#define PRODUCT_ID  SUBSTITUA_PELO_PRODUCTID /* Encontre o ProductID do smartlamp */
static const struct usb_device_id id_table[] = { { USB_DEVICE(VENDOR_ID, PRODUCT_ID) }, {} };

static int  usb_probe(struct usb_interface *ifce, const struct usb_device_id *id); // Executado quando o dispositivo é conectado na USB
static void usb_disconnect(struct usb_interface *ifce);                           // Executado quando o dispositivo USB é desconectado da USB
static int usb_read_serial(void);

// Função para configurar os parâmetros seriais do CP2102 via Control-Messages
static int smartlamp_config_serial(struct usb_device *dev)
{
    int ret;
    u32 baudrate = 9600; // Defina o baud rate que seu ESP32 usa!

    printk(KERN_INFO "SmartLamp: Configurando a porta serial...\n");

    // 1. Habilita a interface UART do CP2102
    //    Comando específico do vendor Silicon Labs (CP210X_IFC_ENABLE)
    //    bmRequestType: 0x41 (Vendor, Host-to-Device, Interface)
    //    bRequest: 0x00 (CP210X_IFC_ENABLE)
    //    wValue: 0x0001 (UART Enable)
    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          0x00, 0x41, 0x0001, 0, NULL, 0, 1000);
    if (ret)
    {
        printk(KERN_ERR "SmartLamp: Erro ao habilitar a UART (código %d)\n", ret);
        return ret;
    }

    // 2. Define o baud rate
    //    Comando específico do vendor Silicon Labs (CP210X_SET_BAUDRATE)
    //    bRequest: 0x1E (CP210X_SET_BAUDRATE)
    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          0x1E, 0x41, 0, 0, &baudrate, sizeof(baudrate), 1000);
    if (ret < 0)
    {
        printk(KERN_ERR "SmartLamp: Erro ao configurar o baud rate (código %d)\n", ret);
        return ret;
    }

    printk(KERN_INFO "SmartLamp: Baud rate configurado para %d\n", baudrate);
    return 0;
}

// Executado quando o arquivo /sys/kernel/smartlamp/{led, ldr} é lido (e.g., cat /sys/kernel/smartlamp/led)
static ssize_t attr_show(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff);
// Executado quando o arquivo /sys/kernel/smartlamp/{led, ldr} é escrito (e.g., echo "100" | sudo tee -a /sys/kernel/smartlamp/led)
static ssize_t attr_store(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count);

// Variáveis para criar os arquivos no /sys/kernel/smartlamp/{led, ldr}
static struct kobj_attribute  led_attribute = __ATTR(led, S_IRUGO | S_IWUSR, attr_show, attr_store);
static struct kobj_attribute  ldr_attribute = __ATTR(ldr, S_IRUGO | S_IWUSR, attr_show, attr_store);
static struct attribute      *attrs[]       = { &led_attribute.attr, &ldr_attribute.attr, NULL };
static struct attribute_group attr_group    = { .attrs = attrs };
static struct kobject        *sys_obj;                                             // Executado para ler a saida da porta serial

MODULE_DEVICE_TABLE(usb, id_table);
int LDR_value = 0;
int LED_value = 0;

static int usb_probe(struct usb_interface *interface, const struct usb_device_id *id);
static void usb_disconnect(struct usb_interface *interface);
static int usb_read_serial(const char *cmd);

static ssize_t attr_show(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff);
static ssize_t attr_store(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count);

static struct kobj_attribute led_attribute = __ATTR(led, S_IRUGO | S_IWUSR, attr_show, attr_store);
static struct kobj_attribute ldr_attribute = __ATTR(ldr, S_IRUGO | S_IWUSR, attr_show, attr_store);
static struct attribute *attrs[] = { &led_attribute.attr, &ldr_attribute.attr, NULL };
static struct attribute_group attr_group = { .attrs = attrs };
static struct kobject *sys_obj;

static struct usb_driver smartlamp_driver = {
    .name        = "smartlamp",
    .probe       = usb_probe,
    .disconnect  = usb_disconnect,
    .id_table    = id_table,
};

module_usb_driver(smartlamp_driver);

static int usb_probe(struct usb_interface *interface, const struct usb_device_id *id) {
    struct usb_endpoint_descriptor *usb_endpoint_in, *usb_endpoint_out;
    int ret;

    printk(KERN_INFO "SmartLamp: Dispositivo conectado ...\n");

    sys_obj = kobject_create_and_add("smartlamp", kernel_kobj);
    if (!sys_obj)
        return -ENOMEM;

    ret = sysfs_create_group(sys_obj, &attr_group);
    if (ret) {
        kobject_put(sys_obj);
        return ret;
    }

    smartlamp_device = interface_to_usbdev(interface);
    ret = usb_find_common_endpoints(interface->cur_altsetting, &usb_endpoint_in, &usb_endpoint_out, NULL, NULL);
    if (ret) {
        kobject_put(sys_obj);
        return ret;
    }

    usb_max_size = usb_endpoint_maxp(usb_endpoint_in);
    usb_in = usb_endpoint_in->bEndpointAddress;
    usb_out = usb_endpoint_out->bEndpointAddress;

    usb_in_buffer = kmalloc(usb_max_size, GFP_KERNEL);
    usb_out_buffer = kmalloc(usb_max_size, GFP_KERNEL);

    // Chama a função para configurar a porta serial antes de usar
    ret = smartlamp_config_serial(smartlamp_device);
    if (ret)
    {
        printk(KERN_ERR "SmartLamp: Falha na configuração da serial\n");
        kfree(usb_in_buffer);
        kfree(usb_out_buffer);
        sysfs_remove_group(sys_obj, &attr_group);
        kobject_put(sys_obj);
        return ret;
    }

    LDR_value = usb_read_serial();

    printk("LDR Value: %d\n", LDR_value);

    return 0;
}

static void usb_disconnect(struct usb_interface *interface) {
    printk(KERN_INFO "SmartLamp: Dispositivo desconectado.\n");
    if (sys_obj) {
        sysfs_remove_group(sys_obj, &attr_group);
        kobject_put(sys_obj);
    }
    kfree(usb_in_buffer);
    kfree(usb_out_buffer);
}
static int usb_read_serial(const char *cmd) {
    int ret, actual_size;
    int retries = 10;
    static char response_buffer[MAX_RECV_LINE]; // Buffer para acumular resposta
    int total_received = 0;

    memset(response_buffer, 0, sizeof(response_buffer));
	
	ret = usb_bulk_msg(smartlamp_device,
                      usb_sndbulkpipe(smartlamp_device, usb_out),
                      (void*)cmd, strlen(cmd), &actual_size, 1000);

	if(ret)
		return ret;

    while (retries > 0) {
        // Lê os dados da porta serial e armazena em usb_in_buffer
        // usb_in_buffer - contem a resposta em string do dispositivo
        // actual_size - contem o tamanho da resposta em bytes
        ret = usb_bulk_msg(smartlamp_device, usb_rcvbulkpipe(smartlamp_device, usb_in), usb_in_buffer, min(usb_max_size, MAX_RECV_LINE), &actual_size, 1000);
        if (ret) {
            printk(KERN_ERR "SmartLamp: Erro ao ler dados (tentativa %d/10). Código: %d\n", 
                   (11 - retries), ret);
            retries--;
            continue;
        }

        // Acumula dados no buffer de resposta
        if (total_received + actual_size < MAX_RECV_LINE) {
            memcpy(response_buffer + total_received, usb_in_buffer, actual_size);
            total_received += actual_size;
            response_buffer[total_received] = '\0'; // Garante terminação nula
        }
		printk(KERN_INFO "SmartLamp: Resposta recebida: [%s]\n", response_buffer);


        // Verifica se recebeu o final da mensagem
        if (strchr(response_buffer, '\n') || strchr(response_buffer, '\r')) {
            break;
        }
    }

    // Processamento final da resposta
    if (total_received > 0) {
        // Remove caracteres de controle e espaços extras
        char *clean_response = strim(response_buffer);
        if(strcmp(cmd, "GET_LDR") == 0){
			// Tenta extrair o valor numérico
        	if (sscanf(clean_response, "RES GET_LDR %d", &LDR_value) == 1) {
            	return LDR_value;
        	}
		}
		else if(strcmp(cmd, "GET_LED") == 0){
			if (sscanf(clean_response, "RES GET_LED %d", &LED_value) == 1) {
            	return LED_value;
        	}


		}
        else {
            printk(KERN_ERR "SmartLamp: Formato de resposta inválido: [%s]\n", clean_response);
            return -EINVAL;
        }
    }

    printk(KERN_ERR "SmartLamp: Nenhum dado válido recebido após 10 tentativas\n");
    return -ETIMEDOUT;
}
// Função principal de leitura serial, com parsing do protocolo correto

static ssize_t attr_show(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff) {
	//nome do arquivo que será criado (ldr ou led)
    const char *attr_name = attr->attr.name;
	//valor do led ou ldr
    int value = -1;

    printk(KERN_INFO "SmartLamp: Lendo %s...\n", attr_name);

    if (strcmp(attr_name, "led") == 0)
        value = usb_read_serial("GET_LED");
    else if (strcmp(attr_name, "ldr") == 0)
        value = usb_read_serial("GET_LDR");
    else
        return -EINVAL;

    if (value < 0)
        return -EIO;

    return sprintf(buff, "%d\n", value);
}

static ssize_t attr_store(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count) {
    long ret, value;
    const char *attr_name = attr->attr.name;

    ret = kstrtol(buff, 10, &value);
    if (ret) {
        printk(KERN_ALERT "SmartLamp: valor de %s invalido.\n", attr_name);
        return -EACCES;
    }

    printk(KERN_INFO "SmartLamp: Setando %s para %ld...\n", attr_name, value);
    return count;
}

