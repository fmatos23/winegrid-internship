/*
 * Origem: escrito de raiz nesta colaboração (Claude), não copiado de
 * nenhum repositório.
 *
 * Prova de conceito do driver out-of-tree para o RN2483 (secção 9.4 do
 * relatório). Sem o módulo físico ligado, esta aplicação valida que o
 * driver compila, regista o dispositivo via devicetree, e é corretamente
 * chamado pela mesma API de LoRa (lora_config()/lora_send()) usada por
 * qualquer transcetor nativo do Zephyr - não valida a comunicação real.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	const struct device *const rn2483 = DEVICE_DT_GET(DT_ALIAS(lora0));

	if (!device_is_ready(rn2483)) {
		printk("RN2483 nao pronto (esperado sem hardware fisico ligado)\n");
	} else {
		printk("RN2483 pronto\n");
	}

	struct lora_modem_config config = {
		.frequency = 868100000,
		.bandwidth = BW_125_KHZ,
		.datarate = SF_7,
		.coding_rate = CR_4_5,
		.preamble_len = 8,
		.tx_power = 14,
		.tx = true,
	};

	int err = lora_config(rn2483, &config);

	if (err) {
		printk("lora_config falhou: %d\n", err);
	}

	while (1) {
		uint8_t payload[] = {0x01, 0x02, 0x03, 0x04};

		err = lora_send(rn2483, payload, sizeof(payload));
		if (err) {
			printk("lora_send falhou: %d\n", err);
		} else {
			printk("lora_send OK\n");
		}

		k_sleep(K_SECONDS(10));
	}

	return 0;
}