#!/usr/bin/python3
import socket
import datetime
import paho.mqtt.client as mqtt
import json

# publish current solar generation data to MQTT broker

# home assistant MQTT auto discovery messages
ha_battery_discover = '''
{
        "name": "Solar battery active power",
        "state_topic": "solar/batteryPower",
        "device_class": "power",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kW",
        "state_class": "measurement",
        "expire_after": 600
}'''

ha_battery_capacity_discover = '''
{
	"name": "Solar battery capacity",
	"state_topic": "solar/batteryCapacitySoc",
	"device_class": "battery",
	"platform": "sensor",
	"unit_of_measurement": "%",
	"expire_after": 600
}
'''

ha_etoday_discover = '''
{
        "name": "Solar generation today",
        "state_topic": "solar/etoday",
        "device_class": "energy",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kWh",
        "state_class": "total_increasing",
        "expire_after": 600,
        "unique_id": "solar_etoday"
}
'''

ha_familyload_discover = '''
{
        "name": "House load power",
        "state_topic": "solar/familyLoadPower",
        "device_class": "power",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kW",
        "state_class": "measurement",
        "expire_after": 600
}
'''

ha_pac_discover = '''
{
        "name": "Solar active power",
        "state_topic": "solar/pac",
        "device_class": "power",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kW",
        "state_class": "measurement",
        "expire_after": 600
}
'''

ha_psum_discover = '''
{
        "name": "Grid active power",
        "state_topic": "solar/psum",
        "device_class": "power",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kW",
        "state_class": "measurement",
        "expire_after": 600
}
'''

ha_battery_charge_discover = '''
{
        "name": "Solar battery charge",
        "state_topic": "solar/batteryTodayChargeEnergy",
        "device_class": "energy",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kWh",
        "state_class": "total_increasing",
        "expire_after": 600
}
'''

ha_battery_discharge_discover = '''
{
        "name": "Solar battery discharge",
        "state_topic": "solar/batteryTodayDischargeEnergy",
        "device_class": "energy",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kWh",
        "state_class": "total_increasing",
        "expire_after": 600
}
'''

ha_grid_purchase_discover = '''
{
        "name": "Grid import",
        "state_topic": "solar/gridPurchasedTodayEnergy",
        "device_class": "energy",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kWh",
        "state_class": "total_increasing",
        "expire_after": 600
}
'''

ha_grid_sell_discover = '''
{
        "name": "Grid export",
        "state_topic": "solar/gridSellTodayEnergy",
        "device_class": "energy",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kWh",
        "state_class": "total_increasing",
        "expire_after": 600
}
'''

def on_mqtt_connect(client, userdata, flags, rc):
    print("broker connect: %d" %(rc))
    if rc!=0:
        exit(rc)

# last solar timestamp it was received at
last_solar_timestamp = None

# connect to mqtt broker 
mqttc = mqtt.Client(client_id="solar-publisher")
mqttc.on_connect = on_mqtt_connect
mqttc.connect("localhost")

# create socket to listen for the broadcast packets
# sent periodically by the solar app
solar_sfd = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
listen_address = ('0.0.0.0',52005)
solar_sfd.bind(listen_address)

mqttc.loop_start()
print("starting loop")

# publish HA MQTT auto-discovery configs
mqttc.publish("homeassistant/sensor/solar/batteryPower/config",ha_battery_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/batteryCapacitySoc/config",ha_battery_capacity_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/etoday/config",ha_etoday_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/familyLoadPower/config",ha_familyload_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/pac/config",ha_pac_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/psum/config",ha_psum_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/batteryTodayChargeEnergy/config",ha_battery_charge_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/batteryTodayDischargeEnergy/config",ha_battery_discharge_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/gridPurchasedTodayEnergy/config",ha_grid_purchase_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/gridSellTodayEnergy/config",ha_grid_sell_discover,retain=True)

while True:
    # wait for and fetch next solar UDP packet
    # this can originate from two places: locally via my ESP/modbus app
    # directly connected to the inverter every 20s but only contains a limited subset
    # of live data
    # Or from the Solis cloud which has more but is only updated every 5mins
    solar_data, address = solar_sfd.recvfrom(1500)
    try:
        json_solar_data = json.loads(str(solar_data,encoding='utf-8'))

        # these nodes are currently only in the packet derived from the cloud data
        if 'batteryTodayChargeEnergy' in json_solar_data['data']:
            batteryTodayChargeEnergy = str(json_solar_data['data']['batteryTodayChargeEnergy'])
            mqttc.publish("solar/batteryTodayChargeEnergy",batteryTodayChargeEnergy)
        if 'batteryTodayDischargeEnergy' in json_solar_data['data']:
            batteryTodayDischargeEnergy = str(json_solar_data['data']['batteryTodayDischargeEnergy'])
            mqttc.publish("solar/batteryTodayDischargeEnergy",batteryTodayDischargeEnergy)
        if 'gridPurchasedTodayEnergy' in json_solar_data['data']:
            gridPurchasedTodayEnergy = str(json_solar_data['data']['gridPurchasedTodayEnergy'])
            mqttc.publish("solar/gridPurchasedTodayEnergy",gridPurchasedTodayEnergy)
        if 'gridSellTodayEnergy' in json_solar_data['data']:
            gridSellTodayEnergy = str(json_solar_data['data']['gridSellTodayEnergy'])
            mqttc.publish("solar/gridSellTodayEnergy",gridSellTodayEnergy)

        # for the 'live' data, make sure we're using the latest
        dataTimestamp = int(json_solar_data['data']['dataTimestamp']) / 1000
        dataTimestamp = datetime.datetime.fromtimestamp(dataTimestamp)
        if last_solar_timestamp==None or dataTimestamp > last_solar_timestamp:
            last_solar_timestamp = dataTimestamp

            # battery charge remaining (in %)
            batteryCapacitySoc = str(int(json_solar_data['data']['batteryCapacitySoc']))
            # current battery power
            batteryPower = json_solar_data['data']['batteryPower']
            batteryPowerStr = json_solar_data['data']['batteryPowerStr']
            # current power from solar
            pac = json_solar_data['data']['pac']
            pacStr = json_solar_data['data']['pacStr']
            # power in/out from grid
            psum = json_solar_data['data']['psum']
            psumStr = json_solar_data['data']['psumStr']
            # current consumption
            familyLoadPower = str(round(json_solar_data['data']['familyLoadPower'],1))
            familyLoadPowerStr = json_solar_data['data']['familyLoadPowerStr']
            # total generation today
            etoday = str(round(json_solar_data['data']['eToday'],1))
            etodayStr = json_solar_data['data']['eTodayStr']

            # flip the psum to align with HA for grid power
            psum = str(round(psum*-1.0,1))
            batteryPower = str(round(batteryPower*-1.0,1))
            pac = str(round(pac,2))

            # publish
            mqttc.publish("solar/batteryCapacitySoc",batteryCapacitySoc)
            mqttc.publish("solar/batteryPower",batteryPower)
            mqttc.publish("solar/pac",pac)
            mqttc.publish("solar/psum",psum)
            mqttc.publish("solar/familyLoadPower",familyLoadPower)
            mqttc.publish("solar/etoday",etoday)
    except:
        print("Exception processing solar data")

mqttc.disconnect()
mqttc.loop_stop()
