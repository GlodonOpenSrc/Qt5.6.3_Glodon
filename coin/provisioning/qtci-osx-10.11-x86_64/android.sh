#!/bin/sh

#############################################################################
##
## Copyright (C) 2016 The Qt Company Ltd.
## Contact: http://www.qt.io/licensing/
##
## This file is part of the test suite of the Qt Toolkit.
##
## $QT_BEGIN_LICENSE:LGPL21$
## Commercial License Usage
## Licensees holding valid commercial Qt licenses may use this file in
## accordance with the commercial license agreement provided with the
## Software or, alternatively, in accordance with the terms contained in
## a written agreement between you and The Qt Company. For licensing terms
## and conditions see http://www.qt.io/terms-conditions. For further
## information use the contact form at http://www.qt.io/contact-us.
##
## GNU Lesser General Public License Usage
## Alternatively, this file may be used under the terms of the GNU Lesser
## General Public License version 2.1 or version 3 as published by the Free
## Software Foundation and appearing in the file LICENSE.LGPLv21 and
## LICENSE.LGPLv3 included in the packaging of this file. Please review the
## following information to ensure the GNU Lesser General Public License
## requirements will be met: https://www.gnu.org/licenses/lgpl.html and
## http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
##
## As a special exception, The Qt Company gives you certain additional
## rights. These rights are described in The Qt Company LGPL Exception
## version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
##
## $QT_END_LICENSE$
##
#############################################################################

# This script install Android sdk and ndk.

# It also runs update for SDK API level 18, latest SDK tools, latest platform-tools and - build-tools

# Android 16 is the minimum requirement for Qt 5.7 applications, but we need something more recent than that for building Qt itself.
# E.g The Bluetooth features that require Android 18 will disable themselves dynamically when running on an Android 16 device.
# That's why we need to use Andoid-18 API version and decision was made to use it also with Qt 5.6.

set -e
targetFolder="/opt/android"
basePath="/net/ci-files01-hki.intra.qt.io/hdd/www/input/android"

# SDK
sdkVersion="android-sdk_r24.4.1-macosx.zip"
sdkBuildToolsVersion="24.0.2"
sdkApiLevel="android-18"
sdkSourceFile="$basePath/$sdkVersion"
sdkExtract="unzip $sdkSourceFile -d $targetFolder"
sdkFolderName="android-sdk-macosx"
sdkName="sdk"

# NDK
ndkVersion="android-ndk-r10e-darwin-x86_64.zip"
ndkSourceFile="$basePath/$ndkVersion"
ndkExtract="unzip $ndkSourceFile -d $targetFolder"
ndkFolderName="android-ndk-r10e"
ndkName="ndk"

function InstallAndroidPackage {
    targetFolder=$1
    version=$2
    extract=$3
    folderName=$4
    name=$5

    sudo $extract || echo "Failed to extract $url"
    sudo chown -R qt:wheel $targetFolder/$folderName
    sudo mv $targetFolder/$folderName $targetFolder/$name || echo "Failed to rename $name"
}

sudo mkdir $targetFolder
# Install Android SDK
echo "Installing Android SDK version $sdkVersion..."
InstallAndroidPackage $targetFolder $sdkVersion "$sdkExtract" $sdkFolderName $sdkName

# Install Android NDK
echo "Installing Android NDK version $ndkVersion..."
InstallAndroidPackage $targetFolder $ndkVersion "$ndkExtract" $ndkFolderName $ndkName

# run update for Android SDK and install SDK API version 18, latest SDK tools, platform-tools and build-tools
echo "Running Android SDK update for API version 18, SDK-tools, platform-tools and build-tools-$sdkBuildToolsVersion..."
echo "y" |$targetFolder/sdk/tools/android update sdk --no-ui --all --filter $sdkApiLevel,tools,platform-tools,build-tools-$sdkBuildToolsVersion || echo "Failed to run update"

# For Qt 5.6, we by default require API levels 10, 11, 16 and 18, but we can override this by setting ANDROID_API_VERSION=android-18
# From Qt 5.7 forward, if android-16 is not installed, Qt will automatically use more recent one.
echo 'export ANDROID_API_VERSION=android-18' >> ~/.bashrc
