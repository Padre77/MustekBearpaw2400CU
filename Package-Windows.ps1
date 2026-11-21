[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$SignedDriverDirectory,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build')
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'build'
$source = (Resolve-Path -LiteralPath $SignedDriverDirectory).ProviderPath
$inf = Join-Path $source 'BearPaw2400CUWia.inf'
$cat = Join-Path $source 'BearPaw2400CUWia.cat'
$cer = Join-Path $source 'BearPaw2400CUWia-test.cer'
$firmware = Join-Path $root 'Driver\PS2fw.usb'
$version = [regex]::Match((Get-Content -Raw -LiteralPath $inf), '(?mi)^DriverVer[ \t]*=[^,\r\n]+,[ \t]*(\d+\.\d+\.\d+\.\d+)[ \t]*\r?$')
if (-not $version.Success) { throw 'Could not read the driver version from the signed INF.' }
if ((Get-FileHash -LiteralPath $firmware).Hash -ne 'F311CAEEA0D31124EEB3D1B90D70F41A430A987B14083F4860483C7DF63BDC72') {
    throw 'Driver\PS2fw.usb does not match the supported firmware.'
}

if ($PSVersionTable.PSEdition -eq 'Desktop') {
    Add-Type -AssemblyName System.Security
} else {
    Add-Type -AssemblyName System.Security.Cryptography.Pkcs
}
$certType = [Security.Cryptography.X509Certificates.X509Certificate2]::GetCertContentType($cer)
if ($certType -ne [Security.Cryptography.X509Certificates.X509ContentType]::Cert) {
    throw 'The signer file must be an exported public certificate (.cer).'
}
$cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($cer)
try {
    $thumbprint = $cert.Thumbprint
} finally {
    $cert.Dispose()
}
$catalogSignature = [Security.Cryptography.Pkcs.SignedCms]::new()
$catalogSignature.Decode([IO.File]::ReadAllBytes($cat))
$catalogSignature.CheckSignature($true)
if ($catalogSignature.SignerInfos.Count -ne 1 -or $catalogSignature.SignerInfos[0].Certificate.Thumbprint -ne $thumbprint) {
    throw 'The catalog is not signed by the supplied certificate.'
}

$package = Join-Path $OutputDirectory ("BearPaw2400CU-{0}-Windows-x64" -f $version.Groups[1].Value)
$archive = "$package.zip"
$driver = Join-Path $package 'Driver'
$app = Join-Path $package 'App'
$payload = @(
    'Driver\BearPaw2400CUWia.inf', 'Driver\BearPaw2400CUWia.dll', 'Driver\BearPaw2400CUWia.cat',
    'Driver\BearPaw2400CUWia-test.cer', 'Driver\PS2fw.usb',
    'App\BearPaw2400CU.exe', 'App\BearPaw2400CUStatus.exe'
)

New-Item -ItemType Directory -Force -Path $driver,$app | Out-Null
foreach ($name in 'BearPaw2400CUWia.inf','BearPaw2400CUWia.dll','BearPaw2400CUWia.cat','BearPaw2400CUWia-test.cer') {
    Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $driver $name) -Force
}
Copy-Item -LiteralPath $firmware -Destination $driver -Force
foreach ($name in 'BearPaw2400CU.exe','BearPaw2400CUStatus.exe') {
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination $app -Force
}
Copy-Item -LiteralPath (Join-Path $root 'INSTALL-Windows.txt') -Destination (Join-Path $package 'INSTALL.txt') -Force

$metadata = Join-Path $package 'PackageMetadata.generated.cs'
$metadataSource = @(
    'internal static partial class InstallDriver',
    '{',
    ('    private const string Thumbprint = "{0}";' -f $thumbprint),
    '    private static readonly System.Collections.Generic.Dictionary<string, string> PackageHashes =',
    '        new System.Collections.Generic.Dictionary<string, string>',
    '        {'
    foreach ($file in $payload) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $package $file)).Hash
        '            {{ @"{0}", "{1}" }},' -f $file, $hash
    }
    '        };',
    '}'
)
Set-Content -LiteralPath $metadata -Value $metadataSource -Encoding ASCII

$installer = Join-Path $package 'Install-BearPaw2400CU.exe'
$compiler = Join-Path $env:windir 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$compilerArgs = @('/nologo','/warnaserror','/codepage:65001','/platform:x64','/target:exe','/optimize+',
    '/reference:System.Management.dll',"/out:$installer",(Join-Path $root 'InstallDriver.cs'),$metadata)
& $compiler @compilerArgs
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }
& $installer --verify
if ($LASTEXITCODE -ne 0) { throw 'The package differs from the verified release payload.' }

$contents = @($installer, (Join-Path $package 'INSTALL.txt'), $driver, $app)
Compress-Archive -LiteralPath $contents -DestinationPath $archive -CompressionLevel Optimal -Force
(Get-Item -LiteralPath $archive).FullName
