#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/string.h>

MODULE_AUTHOR("DevTITANS <devtitans@icomp.ufam.edu.br>");
MODULE_DESCRIPTION("Driver de acesso ao SmartLamp (ESP32 com Chip Serial CP2102)");
MODULE_LICENSE("GPL");

#define MAX_RECV_LINE 100
#define VENDOR_ID   0x10C4
#define PRODUCT_ID  0xEA60

static struct usb_device *smartlamp_device;
static uint usb_in, usb_out;
static char *usb_in_buffer, *usb_out_buffer;
static int usb_max_size;

static const struct usb_device_id id_table[] = {
    { USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
    {}
};

MODULE_DEVICE_TABLE(usb, id_table);

// Prototipos
static int usb_probe(struct usb_interface *ifce, const struct usb_device_id *id);
static void usb_disconnect(struct usb_interface *ifce);
static int usb_send_cmd(const char *cmd, int param);

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

    printk(KERN_INFO "SmartLamp: Endpoints: IN=%02X OUT=%02X\n", usb_in, usb_out);

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

// Comunicação robusta: envia comando e lê resposta acumulando fragmentos até '\n' ou '\r'
static int usb_send_cmd(const char *cmd, int param) {
    int ret, actual_size;
    int retries = 10;
    static char response_buffer[MAX_RECV_LINE];
    int total_received = 0;
    int value = -1;

    // Monta o comando a ser enviado no buffer DMA-safe
    if (param >= 0)
        snprintf(usb_out_buffer, usb_max_size, "%s %d\n", cmd, param);
    else
        snprintf(usb_out_buffer, usb_max_size, "%s\n", cmd);

    // Envia comando usando buffer DMA-safe
    ret = usb_bulk_msg(smartlamp_device,
                       usb_sndbulkpipe(smartlamp_device, usb_out),
                       usb_out_buffer, strlen(usb_out_buffer), &actual_size, 2000);
    if (ret) {
        printk(KERN_ERR "SmartLamp: Erro %d ao enviar comando '%s'\n", ret, cmd);
        return ret;
    }

    memset(response_buffer, 0, sizeof(response_buffer));
    total_received = 0;

    // Lê resposta, acumulando até encontrar \n ou \r
    while (retries-- > 0) {
        ret = usb_bulk_msg(smartlamp_device,
                           usb_rcvbulkpipe(smartlamp_device, usb_in),
                           usb_in_buffer, min(usb_max_size, MAX_RECV_LINE),
                           &actual_size, 1500);

        if (ret) {
            printk(KERN_ERR "SmartLamp: Erro ao ler dados (tentativa %d/10). Código: %d\n",
                   (10 - retries), ret);
            continue;
        }

        if (total_received + actual_size < MAX_RECV_LINE) {
            memcpy(response_buffer + total_received, usb_in_buffer, actual_size);
            total_received += actual_size;
            response_buffer[total_received] = '\0';
        } else {
            printk(KERN_ERR "SmartLamp: Buffer de resposta excedido!\n");
            return -EOVERFLOW;
        }

        printk(KERN_INFO "SmartLamp: Resposta recebida: [%s]\n", response_buffer);

        // Se encontrou fim de linha, para de acumular
        if (strchr(response_buffer, '\n') || strchr(response_buffer, '\r')) {
            break;
        }
    }

    if (total_received > 0) {
        char *clean_resp = strim(response_buffer);
        printk(KERN_INFO "SmartLamp: Resposta processada: [%s]\n", clean_resp);

        if (strncmp(clean_resp, "RES ", 4) == 0) {
            char *cmd_resp = clean_resp + 4;
            if (strncmp(cmd_resp, "GET_LDR ", 8) == 0) {
                if (sscanf(cmd_resp + 8, "%d", &value) == 1)
                    return value;
            } else if (strncmp(cmd_resp, "GET_LED ", 8) == 0) {
                if (sscanf(cmd_resp + 8, "%d", &value) == 1)
                    return value;
            } else if (strncmp(cmd_resp, "SET_LED", 7) == 0) {
                // SET_LED pode retornar o valor setado ou apenas confirmação
                if (sscanf(cmd_resp + 7, "%d", &value) == 1)
                    return value;
                else
                    return 0; // Sucesso genérico
            }
        }

        printk(KERN_ERR "SmartLamp: Formato de resposta inválido!\n");
        return -EINVAL;
    }

    printk(KERN_ERR "SmartLamp: Timeout na leitura da resposta\n");
    return -ETIMEDOUT;
}

// Leitura do sysfs (cat /sys/kernel/smartlamp/led ou ldr)
static ssize_t attr_show(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff) {
    int value = -1;
    const char *attr_name = attr->attr.name;

    printk(KERN_INFO "SmartLamp: Lendo %s ...\n", attr_name);

    if (strcmp(attr_name, "led") == 0)
        value = usb_send_cmd("GET_LED", -1);
    else if (strcmp(attr_name, "ldr") == 0)
        value = usb_send_cmd("GET_LDR", -1);

    if (value < 0)
        return -EIO;

    return sprintf(buff, "%d\n", value);
}

// Escrita no sysfs (echo "100" > /sys/kernel/smartlamp/led)
static ssize_t attr_store(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count) {
    long value;
    const char *attr_name = attr->attr.name;

    if (kstrtol(buff, 10, &value)) {
        printk(KERN_ALERT "SmartLamp: valor de %s invalido.\n", attr_name);
        return -EACCES;
    }

    printk(KERN_INFO "SmartLamp: Setando %s para %ld ...\n", attr_name, value);

    if (strcmp(attr_name, "led") == 0) {
        if (usb_send_cmd("SET_LED", value) < 0)
            return -EIO;
    }

    return count;
}

