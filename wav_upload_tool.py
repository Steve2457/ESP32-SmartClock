#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32 WAV文件上传工具
将WAV文件上传到ESP32的SPIFFS文件系统中

使用方法:
1. 确保ESP32已连接到电脑
2. 将WAV文件放在当前目录的wav_files文件夹中
3. 运行: python wav_upload_tool.py
"""

import os
import sys
import argparse
import wave
import subprocess
import shutil
from pathlib import Path

def check_wav_file(wav_path):
    """检查WAV文件格式和大小"""
    try:
        with wave.open(str(wav_path), 'rb') as wav_file:
            # 获取WAV文件信息
            sample_rate = wav_file.getframerate()
            channels = wav_file.getnchannels()
            sample_width = wav_file.getsampwidth()
            frames = wav_file.getnframes()
            duration = frames / sample_rate
            
            # 计算文件大小
            file_size = wav_path.stat().st_size
            
            print(f"  文件: {wav_path.name}")
            print(f"  大小: {file_size} bytes ({file_size/1024:.1f} KB)")
            print(f"  采样率: {sample_rate} Hz")
            print(f"  声道: {channels}")
            print(f"  位深: {sample_width * 8} bits")
            print(f"  时长: {duration:.2f} 秒")
            
            # 检查文件大小限制
            if file_size > 200 * 1024:  # 200KB
                print(f"  ❌ 警告: 文件大小超过200KB限制!")
                return False
            
            # 检查推荐的音频格式
            if sample_rate not in [22050, 44100, 48000]:
                print(f"  ⚠️  警告: 推荐使用22050Hz、44100Hz或48000Hz采样率")
            
            if channels > 2:
                print(f"  ❌ 错误: 不支持超过2声道的音频")
                return False
            
            if sample_width not in [1, 2]:
                print(f"  ❌ 错误: 只支持8位或16位位深")
                return False
            
            print(f"  ✅ 文件格式检查通过")
            return True
            
    except Exception as e:
        print(f"  ❌ 错误: 无法读取WAV文件 - {e}")
        return False

def find_esp32_port():
    """查找ESP32设备端口"""
    try:
        # 尝试常见的ESP32端口
        import serial.tools.list_ports
        ports = serial.tools.list_ports.comports()
        
        esp32_ports = []
        for port in ports:
            if any(vid_pid in port.hwid.lower() for vid_pid in ['10c4:ea60', '1a86:7523', '0403:6001']):
                esp32_ports.append(port.device)
        
        if esp32_ports:
            print(f"发现ESP32设备端口: {', '.join(esp32_ports)}")
            return esp32_ports[0]
        else:
            print("未发现ESP32设备，请手动指定端口")
            return None
    except ImportError:
        print("请安装pyserial: pip install pyserial")
        return None

def create_spiffs_image(wav_folder, image_path, size_mb=12):
    """创建SPIFFS镜像文件"""
    try:
        # 检查mkspiffs工具
        mkspiffs_path = shutil.which('mkspiffs')
        if not mkspiffs_path:
            print("❌ 未找到mkspiffs工具!")
            print("请从以下网址下载mkspiffs:")
            print("https://github.com/igrr/mkspiffs/releases")
            return False
        
        # 创建SPIFFS镜像 - 使用精确的分区大小
        if size_mb == 12:
            size_bytes = 12517376  # 0xBF0000 - 精确的SPIFFS分区大小
        else:
            size_bytes = size_mb * 1024 * 1024
        cmd = [
            mkspiffs_path,
            '-c', str(wav_folder),
            '-s', str(size_bytes),
            '-p', '256',
            '-b', '4096',
            str(image_path)
        ]
        
        print(f"创建SPIFFS镜像: {image_path}")
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode == 0:
            print("✅ SPIFFS镜像创建成功")
            return True
        else:
            print(f"❌ SPIFFS镜像创建失败: {result.stderr}")
            return False
            
    except Exception as e:
        print(f"❌ 创建SPIFFS镜像时出错: {e}")
        return False

def upload_spiffs_image(image_path, port, offset='0x410000'):
    """上传SPIFFS镜像到ESP32"""
    try:
        # 检查esptool
        esptool_path = shutil.which('esptool.py')
        if not esptool_path:
            print("❌ 未找到esptool.py!")
            print("请安装esptool: pip install esptool")
            return False
        
        # 上传镜像
        cmd = [
            esptool_path,
            '--chip', 'esp32s3',
            '--port', port,
            '--baud', '115200',  # 使用较低波特率避免通信错误
            'write_flash',
            '-z',
            offset,
            str(image_path)
        ]
        
        print(f"上传SPIFFS镜像到ESP32 (端口: {port}, 偏移: {offset})")
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        if result.returncode == 0:
            print("✅ SPIFFS镜像上传成功")
            return True
        else:
            print(f"❌ SPIFFS镜像上传失败:")
            print(f"错误输出: {result.stderr}")
            print(f"标准输出: {result.stdout}")
            print(f"返回码: {result.returncode}")
            return False
            
    except Exception as e:
        print(f"❌ 上传SPIFFS镜像时出错: {e}")
        return False

def main():
    parser = argparse.ArgumentParser(description='ESP32 WAV文件上传工具')
    parser.add_argument('--port', '-p', help='ESP32设备端口 (例如: COM3 或 /dev/ttyUSB0)')
    parser.add_argument('--wav-folder', '-w', default='wav_files', help='WAV文件文件夹路径')
    parser.add_argument('--offset', '-o', default='0x410000', help='SPIFFS分区偏移地址')
    parser.add_argument('--size', '-s', type=int, default=12, help='SPIFFS分区大小 (MB)')
    
    args = parser.parse_args()
    
    print("=" * 60)
    print("           ESP32 WAV文件上传工具")
    print("=" * 60)
    
    # 检查WAV文件夹
    wav_folder = Path(args.wav_folder)
    if not wav_folder.exists():
        print(f"❌ WAV文件夹不存在: {wav_folder}")
        print("请创建wav_files文件夹并放入WAV文件")
        return
    
    # 查找WAV文件
    wav_files = list(wav_folder.glob('*.wav'))
    if not wav_files:
        print(f"❌ 在 {wav_folder} 中未找到WAV文件")
        return
    
    print(f"📁 在 {wav_folder} 中找到 {len(wav_files)} 个WAV文件:")
    print("-" * 40)
    
    # 检查所有WAV文件
    valid_files = []
    total_size = 0
    
    for wav_file in wav_files:
        if check_wav_file(wav_file):
            valid_files.append(wav_file)
            total_size += wav_file.stat().st_size
        print()
    
    if not valid_files:
        print("❌ 没有有效的WAV文件")
        return
    
    print(f"✅ 有效文件: {len(valid_files)} 个")
    print(f"📊 总大小: {total_size} bytes ({total_size/1024:.1f} KB)")
    
    # 检查总大小
    if total_size > args.size * 1024 * 1024:
        print(f"❌ 总文件大小超过SPIFFS分区大小 ({args.size}MB)")
        return
    
    # 查找ESP32端口
    port = args.port
    if not port:
        port = find_esp32_port()
        if not port:
            print("请使用 --port 参数指定ESP32设备端口")
            return
    
    # 创建SPIFFS镜像
    image_path = Path('spiffs_image.bin')
    if not create_spiffs_image(wav_folder, image_path, args.size):
        return
    
    # 上传SPIFFS镜像
    if upload_spiffs_image(image_path, port, args.offset):
        print()
        print("🎉 WAV文件上传成功!")
        print("📋 上传的文件:")
        for wav_file in valid_files:
            print(f"   - {wav_file.name}")
        print()
        print("💡 提示:")
        print("   1. 重启ESP32设备")
        print("   2. 查看串口日志确认文件是否被正确加载")
        print("   3. 在代码中使用 audio_play_wav_file(\"文件名.wav\", 音量) 播放")
    
    # 清理临时文件
    if image_path.exists():
        image_path.unlink()

if __name__ == '__main__':
    main() 