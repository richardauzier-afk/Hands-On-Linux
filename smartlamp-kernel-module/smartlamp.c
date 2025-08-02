#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/ctype.h>

MODULE_AUTHOR("DevTITANS <devtitans@icomp.ufam.edu.br>");
MODULE_DESCRIPTION("Driver de acesso ao SmartLamp (ESP32 com Chip Serial CP2102)");
MODULE_LICENSE("GPL");

#define MAX_RECV_LINE 100
#define VENDOR_ID   0x10C4
#define PRODUCT_ID  0xEA60
#define MAX_NUM_STR_SIZE 32

static struct usb_device *smartlamp_device;
static uint usb_in, usb_out;
static char *usb_in_buffer, *usb_out_buffer;
static int usb_max_size;

static const struct usb_device_id id_table[] = {
    { USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
    {}
};

static int usb_probe(struct usb_interface *ifce, const struct usb_device_id *id);
static void usb_disconnect(struct usb_interface *ifce);
static long usb_send_cmd(const char *cmd, int param);

static ssize_t attr_show(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff);
static ssize_t attr_store(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count);

static struct kobj_attribute led_attribute = __ATTR(led, S_IRUGO | S_IWUSR, attr_show, attr_store);
static struct kobj_attribute ldr_attribute = __ATTR(ldr, S_IRUGO | S_IWUSR, attr_show, attr_store);
static struct kobj_attribute temp_attribute = __ATTR(temp, S_IRUGO | S_IWUSR, attr_show, attr_store);
static struct kobj_attribute hum_attribute = __ATTR(hum, S_IRUGO | S_IWUSR, attr_show, attr_store);


static struct attribute *attrs[] = { &led_attribute.attr, &ldr_attribute.attr, &temp_attribute.attr, &hum_attribute.attr, NULL };
static struct attribute_group attr_group = { .attrs = attrs };
static struct kobject *sys_obj;

static struct usb_driver smartlamp_driver = {
    .name        = "smartlamp",
    .probe       = usb_probe,
    .disconnect  = usb_disconnect,
    .id_table    = id_table,
};

module_usb_driver(smartlamp_driver);

static int smartlamp_config_serial(struct usb_device *dev)
{
    int ret;
    u32 baudrate = 9600;

    printk(KERN_INFO "SmartLamp: Configurando a porta serial...\n");

    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          0x00, 0x41, 0x0001, 0, NULL, 0, 1000);
    if (ret) {
        printk(KERN_ERR "SmartLamp: Erro ao habilitar a UART (código %d)\n", ret);
        return ret;
    }

    ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
                          0x1E, 0x41, 0, 0, &baudrate, sizeof(baudrate), 1000);
    if (ret < 0) {
        printk(KERN_ERR "SmartLamp: Erro ao configurar o baud rate (código %d)\n", ret);
        return ret;
    }

    printk(KERN_INFO "SmartLamp: Baud rate configurado para %d\n", baudrate);
    return 0;
}

MODULE_DEVICE_TABLE(usb, id_table);

// Extrai último número (int ou float) como string
static int extrair_ultimo_numero_str(const char *str, char *num_str, size_t sz)
{
    const char *p = str;
    const char *ultimo_ini = NULL;
    const char *ultimo_fim = NULL;

    while (*p) {
        if ((*p == '+' || *p == '-' || isdigit(*p))) {
            const char *start = p;
            p++;
            while (*p && (isdigit(*p) || *p == '.'))
                p++;
            ultimo_ini = start;
            ultimo_fim = p;
        } else {
            p++;
        }
    }
    if (ultimo_ini && ultimo_fim) {
        size_t len = ultimo_fim - ultimo_ini;
        if (len >= sz)
            len = sz-1;
        strncpy(num_str, ultimo_ini, len);
        num_str[len] = '\0';
        return 0;
    }
    num_str[0] = '\0';
    return -1;
}

// Extrai último número, devolve int se possível, ou float*1000 (milésimos)
static long extrair_ultimo_numero_kernel(const char *str)
{
    char num_str[MAX_NUM_STR_SIZE];
    int int_val;
    long fixed_val;

    if (extrair_ultimo_numero_str(str, num_str, sizeof(num_str)) < 0)
        return -EINVAL;

    if (kstrtoint(num_str, 10, &int_val) == 0)
        return int_val;

    if (strchr(num_str, '.')) {
        long inteiro = 0, mil = 0;
        char temp[16];
        char *dot = strchr(num_str, '.');
        int intlen = dot - num_str;
        int n;

        if (intlen > 15)
            intlen = 15;
        strncpy(temp, num_str, intlen);
        temp[intlen] = '\0';

        if (kstrtol(temp, 10, &inteiro))
            return -EINVAL;
        if (kstrtol(dot+1, 10, &mil))
            return -EINVAL;

        n = strlen(dot+1);
        while (n < 3) { mil *= 10; n++; }
        while (n > 3) { mil /= 10; n--; }
        if (inteiro < 0)
            fixed_val = inteiro*1000 - mil;
        else
            fixed_val = inteiro*1000 + mil;
        return fixed_val;
    }
    return -EINVAL;
}

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
    if (!usb_in_buffer || !usb_out_buffer) {
        ret = -ENOMEM;
        goto fail_buffers;
    }

    ret = smartlamp_config_serial(smartlamp_device);
    if (ret) {
        printk(KERN_ERR "SmartLamp: Falha na configuração da serial\n");
        goto fail_buffers;
    }

    return 0;

fail_buffers:
    kfree(usb_in_buffer);
    kfree(usb_out_buffer);
    sysfs_remove_group(sys_obj, &attr_group);
    kobject_put(sys_obj);
    return ret;
}

static void usb_disconnect(struct usb_interface *interface) {
    printk(KERN_INFO "SmartLamp: Dispositivo desconectado.\n");
    if (sys_obj) {
        sysfs_remove_group(sys_obj, &attr_group);
        kobject_put(sys_obj);
        sys_obj = NULL;
    }
    kfree(usb_in_buffer);
    kfree(usb_out_buffer);
    usb_in_buffer = NULL;
    usb_out_buffer = NULL;
}

static long usb_send_cmd(const char *cmd, int param) {
    char *clean_resp;
    int ret, actual_size;
    int retries = 10;
    static char response_buffer[MAX_RECV_LINE];
    int total_received = 0;
    long num;

    if (!smartlamp_device)
        return -ENODEV;

    if (param >= 0)
        snprintf(usb_out_buffer, usb_max_size, "%s %d\n", cmd, param);
    else
        snprintf(usb_out_buffer, usb_max_size, "%s\n", cmd);

    ret = usb_bulk_msg(smartlamp_device,
                       usb_sndbulkpipe(smartlamp_device, usb_out),
                       usb_out_buffer, strlen(usb_out_buffer), &actual_size, 2000);
    if (ret) {
        printk(KERN_ERR "SmartLamp: Erro %d ao enviar comando '%s'\n", ret, cmd);
        return ret;
    }

    memset(response_buffer, 0, sizeof(response_buffer));
    total_received = 0;

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

        if (strchr(response_buffer, '\n') || strchr(response_buffer, '\r')) {
            break;
        }
    }

    if (total_received > 0) {
        clean_resp = strim(response_buffer);
        printk(KERN_INFO "SmartLamp: Resposta processada: [%s]\n", clean_resp);

        num = extrair_ultimo_numero_kernel(clean_resp);
        if (num != -EINVAL)
            return num;

        printk(KERN_ERR "SmartLamp: Formato de resposta inválido!\n");
        return -EINVAL;
    }

    printk(KERN_ERR "SmartLamp: Timeout na leitura da resposta\n");
    return -ETIMEDOUT;
}

static ssize_t attr_show(struct kobject *sys_obj, struct kobj_attribute *attr, char *buff) {
    long value = -1;
    const char *attr_name = attr->attr.name;
    long aval;

    printk(KERN_INFO "SmartLamp: Lendo %s ...\n", attr_name);

    if (strcmp(attr_name, "led") == 0)
        value = usb_send_cmd("GET_LED", -1);
    else if (strcmp(attr_name, "ldr") == 0)
        value = usb_send_cmd("GET_LDR", -1);
    else if (strcmp(attr_name, "temp") == 0)
        value = usb_send_cmd("GET_TEMP", -1);
    else if (strcmp(attr_name, "hum") == 0)
        value = usb_send_cmd("GET_HUM", -1);
    

    if (value < 0)
        return -EIO;

    if (value % 1000 == 0)
        return sprintf(buff, "%ld\n", value/1000);
    else {
        aval = value;
        if (aval < 0) {
            aval = -aval;
            return sprintf(buff, "-%ld.%03ld\n", value/1000, aval%1000);
        } else {
            return sprintf(buff, "%ld.%03ld\n", value/1000, value%1000);
        }
    }
}

static ssize_t attr_store(struct kobject *sys_obj, struct kobj_attribute *attr, const char *buff, size_t count) {
    long value;
    const char *attr_name = attr->attr.name;

    if (kstrtol(buff, 10, &value)) {
        printk(KERN_ALERT "SmartLamp: valor de %s inválido.\n", attr_name);
        return -EACCES;
    }

    printk(KERN_INFO "SmartLamp: Setando %s para %ld ...\n", attr_name, value);

    if (strcmp(attr_name, "led") == 0) {
        if (usb_send_cmd("SET_LED", value) < 0)
            return -EIO;
    }

    return count;
}
