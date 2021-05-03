#!/bin/bash

#############################################################################
##
## Copyright (C) 2017 The Qt Company Ltd.
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

# A helper script used for downloading a file from a URL or an alternative
# URL. Also the SHA1 is checked for the file. Target filename should also
# be given.
#
# If called directly from another script, it will exit the parent script
# as well, if not called in its own subshell with parentheses.

# shellcheck source=try_catch.sh
source "${BASH_SOURCE%/*}/try_catch.sh"

ExceptionDownloadPrimaryUrl=100
ExceptionDownloadAltUrl=101
ExceptionSHA1=102

function DownloadURL {
    url=$1
    url_alt=$2
    expectedSha1=$3
    targetFile=$4

    try
    (
        try
        (
            echo "Downloading from primary URL '$url'"
            curl --fail -L --retry 5 --retry-delay 5 -o "$targetFile" "$url" || throw $ExceptionDownloadPrimaryUrl
        )
        catch || {
            case $ex_code in
                $ExceptionDownloadPrimaryUrl)
                    echo "Failed to download '$url' multiple times"
                    echo "Downloading tar.gz from alternative URL '$url_alt'"
                    curl --fail -L --retry 5 --retry-delay 5 -o "$targetFile" "$url_alt" || throw $ExceptionDownloadAltUrl
                ;;
            esac
        }
        echo "Checking SHA1 on PKG '$targetFile'"
        echo "$expectedSha1 *$targetFile" | shasum --check || throw $ExceptionSHA1
    )

    catch || {
        case $ex_code in
            $ExceptionDownloadAltUrl)
                echo "Failed downloading PKG from primary and alternative URLs"
                exit 1;
            ;;
            $ExceptionSHA1)
                echo "Failed checksum on $targetFile."
                exit 1;
            ;;
        esac
    }
}

