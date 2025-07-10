#!/bin/bash

# 下载LVGL库的脚本
echo "正在下载LVGL库..."

# 创建components/lvgl目录
mkdir -p components/lvgl

# 进入lvgl目录
cd components/lvgl

# 克隆LVGL仓库 (使用8.3版本，这是一个稳定版本)
if [ ! -d "lvgl" ]; then
    echo "正在克隆LVGL仓库..."
    git clone --branch release/v8.3 --depth 1 https://github.com/lvgl/lvgl.git
    echo "LVGL库下载完成"
else
    echo "LVGL库已存在"
fi

# 返回项目根目录
cd ../..

echo "LVGL设置完成！"
echo "现在您可以运行 'idf.py build' 来编译项目" 