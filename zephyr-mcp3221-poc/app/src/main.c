/*
 * Origem: escrito de raiz nesta colaboração (Claude), não copiado de
 * nenhum repositório.
 *
 * Prova de conceito do driver out-of-tree para o MCP3221 (secção 3.5/8.4
 * do relatório). Sem o chip físico ligado, esta aplicação valida que o
 * driver compila, regista o dispositivo via devicetree e é corretamente
 * chamado pela API de sensores do Zephyr - não valida a leitura real do
 * ADC.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/sys/printk.h>

int main(void)
{
	const struct device *const mcp3221 = DEVICE_DT_GET(DT_NODELABEL(mcp3221_0));

	if (!device_is_ready(mcp3221)) {
		printk("MCP3221 nao pronto (esperado sem hardware fisico ligado)\n");
	} else {
		printk("MCP3221 pronto\n");
	}

	while (1) {
		int err = sensor_sample_fetch(mcp3221);

		if (err == 0) {
			struct sensor_value val;

			sensor_channel_get(mcp3221, SENSOR_CHAN_VOLTAGE, &val);
			printk("MCP3221 tensao: %d.%06d V\n", val.val1, val.val2);
		} else {
			printk("sensor_sample_fetch falhou: %d\n", err);
		}

		k_sleep(K_SECONDS(2));
	}

	return 0;
}