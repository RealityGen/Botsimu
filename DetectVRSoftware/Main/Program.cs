﻿using System;
using System.IO;
using System.Threading;


namespace DetectVR
{
    class Program
    {
        public static readonly string READ_FILE = Directory.GetCurrentDirectory() + "/ReadHere.ini";
        public static XInputController controller = new XInputController();
        public static Arduino arduino;
        public static PlayerProperties player = PlayerProperties.PlayerOne;

        static void Main(string[] args)
        {
            while (controller.connected)
            {
                controller.Update();

                SendCommandsToArduino();
            }
        }

        public static void WriteJoySticksValue(int delay)
        {
            Console.WriteLine("Left stick x value: " + controller.leftThumb.x);
            Console.WriteLine("Left stick y value: " + controller.leftThumb.y);

            Console.WriteLine("Right stick x value: " + controller.rightThumb.x);
            Console.WriteLine("Right stick y value: " + controller.rightThumb.y);

            Console.WriteLine();

            Thread.Sleep(delay);
        }

        public static void WriteToFileYValue(string path)
        {
            using (StreamWriter writetext = new StreamWriter(path))
            {
                writetext.WriteLine("Value: " + controller.leftThumb.y);
                Console.WriteLine("Value written in " + path + ": " + controller.leftThumb.y);
            }
        }

        public static void SendCommandsToArduino()
        {
            float y = controller.leftThumb.y;
            float voltage = 0f;

            if (arduino == null)
            {
                arduino = new Arduino();
            }

            if (y <= 40f && !player.IsNotMoving)
            {
                arduino.Data[0] = Convert.ToByte(4);
                arduino.Port.Write(arduino.Data, 0, 1);
                voltage = 4f / 255f;

                player.IsNotMoving = true;
                player.IsWalking = false;
                player.IsJogging = false;
                player.IsSprinting = false;

                Console.WriteLine(voltage + " volts sent to port." + arduino.Port.PortName);
            }
            if (y < 70f && y > 40f && !player.IsWalking)
            {
                arduino.Data[0] = Convert.ToByte(20);
                arduino.Port.Write(arduino.Data, 0, 1);
                voltage = 20f / 255f;

                player.IsNotMoving = false;
                player.IsWalking = true;
                player.IsJogging = false;
                player.IsSprinting = false;

                Console.WriteLine(voltage + " volts sent to port." + arduino.Port.PortName);
            }
            if (y > 70f && !player.IsJogging)
            {
                arduino.Data[0] = Convert.ToByte(50);
                arduino.Port.Write(arduino.Data, 0, 1);
                voltage = 50f / 255f;

                player.IsNotMoving = false;
                player.IsWalking = false;
                player.IsJogging = true;
                player.IsSprinting = false;

                Console.WriteLine(voltage + " volts sent to port." + arduino.Port.PortName);
            }

            // TODO Sprint
            //if (y > 80 && OVRInput.Get(OVRInput.Button.PrimaryThumbstick))
            //{
            //    ArduinoManager.Board.analogWrite((int)ArduinoManager.Pin.Speed, 80);
            //}

        }
    }
}