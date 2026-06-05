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
        "expire_after": 900
}'''

ha_battery_capacity_discover = '''
{
	"name": "Solar battery capacity",
	"state_topic": "solar/batteryCapacitySoc",
	"device_class": "battery",
	"platform": "sensor",
	"unit_of_measurement": "%",
	"expire_after": 900
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
        "expire_after": 900,
        "unique_id": "solar_etoday"
}
'''

ha_etotal_discover = '''
{
        "name": "Solar generation total",
        "state_topic": "solar/etotal",
        "device_class": "energy",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kWh",
        "state_class": "total",
        "expire_after": 900,
        "unique_id": "solar_etotal"
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
        "expire_after": 900
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
        "expire_after": 900
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
        "expire_after": 900
}
'''

ha_battery_charge_discover = '''
{
        "name": "Solar battery charge",
        "state_topic": "solar/batteryTotalChargeEnergy",
        "device_class": "energy",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kWh",
        "state_class": "total",
        "expire_after": 900
}
'''

ha_battery_discharge_discover = '''
{
        "name": "Solar battery discharge",
        "state_topic": "solar/batteryTotalDischargeEnergy",
        "device_class": "energy",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kWh",
        "state_class": "total",
        "expire_after": 900
}
'''

ha_grid_purchase_discover = '''
{
        "name": "Grid import",
        "state_topic": "solar/gridPurchasedTotalEnergy",
        "device_class": "energy",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kWh",
        "state_class": "total",
        "expire_after": 900
}
'''

ha_grid_sell_discover = '''
{
        "name": "Grid export",
        "state_topic": "solar/gridSellTotalEnergy",
        "device_class": "energy",
        "suggested_display_precision": 1,
        "platform": "sensor",
        "unit_of_measurement": "kWh",
        "state_class": "total",
        "expire_after": 900
}
'''

ha_logger_fails_discover = '''
{
        "name": "Solis data logger failure count",
        "state_topic": "solar/solisLoggerFailureCount",
        "platform": "sensor",
        "state_class": "total_increasing",
        "expire_after": 900
}
'''

def convert_units(value,actual_units,required_units):
    if actual_units==required_units:
        return str(value)

    # convert to W - lowest common denominator
    if 'kW' in actual_units:
        value = float(value)*1000
    elif 'MW' in actual_units:
        value = float(value)*1000*1000
    elif 'GW' in actual_units:
        value = float(value)*1000*1000*1000
    else:
        value = float(value) # assumed just 'W', unlikely
        
    # convert to desired units
    if 'kW' in required_units:
        value = str(round(value/1000,1))
    elif 'MW' in required_units:
        value = str(round(value/1000/1000,2))
    elif 'GW' in required_units:
        value = str(round(value/1000/1000/1000,3))
    else:
        value = str(value)
    
    return value
    
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
mqttc.publish("homeassistant/sensor/solar/etotal/config",ha_etotal_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/familyLoadPower/config",ha_familyload_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/pac/config",ha_pac_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/psum/config",ha_psum_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/batteryTotalChargeEnergy/config",ha_battery_charge_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/batteryTotalDischargeEnergy/config",ha_battery_discharge_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/gridPurchasedTotalEnergy/config",ha_grid_purchase_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/gridSellTotalEnergy/config",ha_grid_sell_discover,retain=True)
mqttc.publish("homeassistant/sensor/solar/solisLoggerFailureCount/config",ha_logger_fails_discover,retain=True)

last_etotal = "0.0"
last_batteryTotalChargeEnergy = "0"
last_batteryTotalDischargeEnergy = "0"
last_gridPurchasedTotalEnergy = "0"
last_gridSellTotalEnergy = "0"

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
        if 'batteryTotalChargeEnergy' in json_solar_data['data']:
            batteryTotalChargeEnergy = convert_units(json_solar_data['data']['batteryTotalChargeEnergy'],
                                                     json_solar_data['data']['batteryTotalChargeEnergyStr'],
                                                                             "kWh")
            if batteryTotalChargeEnergy>=last_batteryTotalChargeEnergy:
                mqttc.publish("solar/batteryTotalChargeEnergy",batteryTotalChargeEnergy)
                last_batteryTotalChargeEnergy=batteryTotalChargeEnergy
        if 'batteryTotalDischargeEnergy' in json_solar_data['data']:
            batteryTotalDischargeEnergy = convert_units(json_solar_data['data']['batteryTotalDischargeEnergy'],
                                                        json_solar_data['data']['batteryTotalDischargeEnergyStr'],
                                                        "kWh")
            if batteryTotalDischargeEnergy>=last_batteryTotalDischargeEnergy:
                mqttc.publish("solar/batteryTotalDischargeEnergy",batteryTotalDischargeEnergy)
                last_batteryTotalDischargeEnergy = batteryTotalDischargeEnergy
        if 'gridPurchasedTotalEnergy' in json_solar_data['data']:
            gridPurchasedTotalEnergy = convert_units(json_solar_data['data']['gridPurchasedTotalEnergy'],
                                                     json_solar_data['data']['gridPurchasedTotalEnergyStr'],
                                                     "kWh")
            if gridPurchasedTotalEnergy>=last_gridPurchasedTotalEnergy:
                mqttc.publish("solar/gridPurchasedTotalEnergy",gridPurchasedTotalEnergy)
                last_gridPurchasedTotalEnergy = gridPurchasedTotalEnergy
        if 'gridSellTotalEnergy' in json_solar_data['data']:
            gridSellTotalEnergy = convert_units(json_solar_data['data']['gridSellTotalEnergy'],
                                                json_solar_data['data']['gridSellTotalEnergyStr'],
                                                "kWh")
            if gridSellTotalEnergy>=last_gridSellTotalEnergy:
                mqttc.publish("solar/gridSellTotalEnergy",gridSellTotalEnergy)
                last_gridSellTotalEnergy = gridSellTotalEnergy
        if 'eTotal' in json_solar_data['data']:
            eTotal = convert_units(json_solar_data['data']['eTotal'],json_solar_data['data']['eTotalStr'],"kWh")
            # temp workaround for differential between local/cloud figures
            if eTotal>=last_etotal:
                mqttc.publish("solar/etotal",eTotal)
                last_etotal = eTotal
        # this is ONLY in the data published locally and provides a counter
        # of how many times the modbus app detects that the logger has stopped issuing requests
        if 'loggerFail' in json_solar_data:
            loggerFail = str(json_solar_data['loggerFail'])
            mqttc.publish("solar/solisLoggerFailureCount",loggerFail)

        # for the 'live' data, make sure we're using the latest
        dataTimestamp = int(json_solar_data['data']['dataTimestamp']) / 1000
        dataTimestamp = datetime.datetime.fromtimestamp(dataTimestamp)
        if last_solar_timestamp==None or dataTimestamp >= last_solar_timestamp:
            last_solar_timestamp = dataTimestamp

            # battery charge remaining (in %)
            batteryCapacitySoc = str(int(json_solar_data['data']['batteryCapacitySoc']))

            # current battery power
            # flip the battery power to align with HA for grid power
            batteryPower = convert_units(json_solar_data['data']['batteryPower']*-1,
                                         json_solar_data['data']['batteryPowerStr'],
                                         "kW")
            # current power from solar
            pac = convert_units(json_solar_data['data']['pac'],
                                json_solar_data['data']['pacStr'],
                                "kW")
            # power in/out from grid
            # flip the psum to align with HA for grid power
            psum = convert_units(json_solar_data['data']['psum']*-1,
                                 json_solar_data['data']['psumStr'],
                                 "kW")
            # current consumption
            familyLoadPower = convert_units(json_solar_data['data']['familyLoadPower'],
                                            json_solar_data['data']['familyLoadPowerStr'],
                                            "kW")
            # total generation today
            etoday = convert_units(json_solar_data['data']['eToday'],
                                   json_solar_data['data']['eTodayStr'],
                                   "kWh")
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
