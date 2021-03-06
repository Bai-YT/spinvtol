import serial
from time import sleep
from struct import unpack,pack
import binascii
avionicsCommPort = '/dev/ttyACM0'
try:
    with serial.Serial(avionicsCommPort,115200, timeout=0.1) as avionics:
        avionics.reset_input_buffer()
        while True:
            if (False and avionics.in_waiting > 0 ):
                print(avionics.in_waiting)
                data = avionics.read(8)
                #msgType = unpack('B',data[0])
                msgType = data[0]
                voltage = unpack('H',data[1:3])[0]/100.0
                flapPWM = unpack('H',data[3:5])[0]
                throttlePWM = unpack('H',data[5:7])[0]
                isTelemCtrl = data[7]
                print(msgType,voltage,flapPWM,throttlePWM,isTelemCtrl)
            if ( avionics.in_waiting>0):
                line = avionics.readline()
                try:
                    print("avionics: "+line.decode('ascii'))
                except UnicodeDecodeError:
                    pass

            outdata = bytearray(5)
            # message type
            outdata[0] = 1
            # flap pwm
            outdata[1:3] = pack('H',1550)
            # throttle pwm
            outdata[3:] = pack('H',1606)

            #outcount = avionics.write(outdata)
            #print('out '+str(outcount)+':'+binascii.hexlify(outdata))
            #sleep(0.01)
except serial.serialutil.SerialException as e:
    print(e)
