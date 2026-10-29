# PhonoStethos — Cheatsheet (comandos rápidos)

##  FLASH

```bash
# ============================================================
# 1. BUILD (só se alteraste o .config ou o overlay)
# ============================================================
cd /home/igor/buildroot/buildroot-2026.08
rm -f output/images/rootfs.ext2 output/images/rootfs.tar \
      output/images/rootfs.tar.bz2 output/images/sdcard.img
make

# ============================================================
# 2. VERIFICAR IMAGEM (loop mount, antes de flashar)
# ============================================================
cd /home/igor/buildroot/buildroot-2026.08
sudo losetup -Pf output/images/sdcard.img
LOOPDEV=$(losetup -j output/images/sdcard.img | cut -d: -f1)
echo "Loop: $LOOPDEV"

sudo mkdir -p /mnt/img_root
sudo mount ${LOOPDEV}p2 /mnt/img_root

# confirmar o que esperas ver
ls /mnt/img_root/etc/ssh/ssh_host_*
sudo stat -c "%a %U:%G %n" /mnt/img_root/etc/ssh/ssh_host_*
ls /mnt/img_root/etc/init.d/S11gadget
cat /mnt/img_root/etc/network/interfaces
cat /mnt/img_root/etc/asound.conf

sudo umount /mnt/img_root
sudo losetup -d $LOOPDEV

# ============================================================
# 3. FLASHAR
# ============================================================
cd /home/igor/buildroot/buildroot-2026.08
sudo umount /dev/sda1 /dev/sda2 2>/dev/null
sudo umount /mnt/sd_boot /mnt/sd_root /mnt/img_root 2>/dev/null

sudo dd if=output/images/sdcard.img of=/dev/sda bs=4M status=progress conv=fsync
sync
sync
sleep 5

# ============================================================
# 4. VERIFICAR HASH (obrigatório)
# ============================================================
SIZE=$(stat -c%s output/images/sdcard.img)
sha256sum output/images/sdcard.img
sudo head -c $SIZE /dev/sda | sha256sum
# Os dois hashes TÊM DE SER IGUAIS. Se não forem, cartão mau → trocar.

# ============================================================
# 5. EJETAR
# ============================================================
sudo eject /dev/sda

# ============================================================
# 6. CARTÃO NA PI + CABO USB-C + ESPERAR 40 s
# ============================================================

# ============================================================
# 7. NO PC: LIMPAR KNOWN_HOSTS E ACEITAR A CHAVE FIXA
# ============================================================
ssh-keygen -R paran 2>/dev/null
ssh-keygen -R 10.42.0.3 2>/dev/null
ssh-keygen -R kalam 2>/dev/null
ssh-keygen -R 10.42.0.2 2>/dev/null

ssh-keyscan -H 10.42.0.3 >> ~/.ssh/known_hosts 2>/dev/null
ssh-keyscan -t ed25519 10.42.0.3 2>/dev/null | ssh-keygen -lf -
# esperado: SHA256:ZBNJHkTVUHH07wFg630gzDMF3dtNS6+k52gZ901kY1U

# ============================================================
# 8. TESTAR
# ============================================================
ssh root@paran
# password: pass
```

---

## 📦 COPIAR FICHEIROS PARA A PI (scp)

```bash
# AlsaTest
scp -p ~/Desktop/DD_AlsaTest/AlsaTest root@paran:/root/

# phonostethos (quando compilado)
scp -p ~/Desktop/phonostethos/build/phonostethos root@paran:/root/

# driver LEDs (quando compilado)
scp -p ~/Desktop/kernel/phonostethos_leds.ko root@paran:/root/

# ficheiro qualquer
scp -p ficheiro.ext root@paran:/root/

# pasta inteira
scp -rp pasta/ root@paran:/root/
```

---

## 🎵 TESTAR ÁUDIO (na Pi)

```bash
# 1. ver os cards
cat /proc/asound/cards
# esperado: HiFiAudio (USB mic) + Headphones (jack Pi)

# 2. gravar 5 s (usa o asound.conf → mic USB automaticamente)
AlsaTest rec /root/test.wav 5

# 3. verificar que gravou áudio (não silêncio)
xxd -s 44 -l 64 /root/test.wav
# valores variados, NÃO todos 0000

# 4. ouvir no jack da Pi (usa asound.conf → jack da Pi)
AlsaTest play /root/test.wav

# se o "default" não funcionar, usar explicitamente:
AlsaTest rec /root/test.wav 5 plughw:CARD=HiFiAudio,DEV=0
AlsaTest play /root/test.wav plughw:CARD=Headphones,DEV=0
```

---

## 🐛 TROUBLESHOOTING RÁPIDO

### "Não consigo entrar por SSH"
```bash
# 1. o gadget USB existe?
lsusb | grep -iE "ethernet|rndis"
# vazio → power insuficiente no USB-C, ou Pi não arrancou

# 2. a interface tem o nome fixo?
ip -br link show | grep enx020000000001

# 3. o NM ligou o perfil?
nmcli connection show --active | grep phonostethos

# 4. o IP está atribuído?
ip addr show enx020000000001 | grep "inet "
# esperado: inet 10.42.0.10/24

# 5. ping direto?
ping -c 3 10.42.0.3

# 6. forçar o perfil NM
sudo nmcli connection up phonostethos-usb
```

### "Permission denied" na password
```bash
# O sshd_config não está a permitir root com password
ssh root@paran "grep -E '^PermitRootLogin|^PasswordAuthentication' /etc/ssh/sshd_config"
# esperado: PermitRootLogin yes + PasswordAuthentication yes
```

### "The authenticity of host... can't be established" (fingerprint mudou)
```bash
ssh-keygen -R paran
ssh-keygen -R 10.42.0.3
ssh-keyscan -H 10.42.0.3 >> ~/.ssh/known_hosts 2>/dev/null
ssh-keyscan -t ed25519 10.42.0.3 2>/dev/null | ssh-keygen -lf -
# se NÃO for ZBNJHkTV..., o overlay com as chaves não foi aplicado
```

### "Structure needs cleaning" ao montar o cartão no PC
- Cartão corrompido (tirar sem sync, ou corte de power)
- **Não tentar reparar** — re-flashar:
  ```bash
  sudo dd if=output/images/sdcard.img of=/dev/sda bs=4M status=progress conv=fsync
  sync; sync; sleep 5
  ```

### Pi não arranca (LED verde não pisca)
- **Power insuficiente** — Pi 4 precisa 3A. USB-C de laptop dá ~1.5A
- Solução: fonte 5V/3A **via GPIO** (pino 2 = 5V, pino 6 = GND) + cabo USB-C ao PC só para dados
- Ou: fonte 5V/3A no USB-C, mas aí perdes o gadget USB

---

## 🔧 CONFIGURAÇÃO DO PC (uma vez só)

### NetworkManager (plug-and-play)
```bash
# apagar duplicadas se existirem
nmcli connection show | grep phonostethos
nmcli connection delete <UUID>

# criar perfil
sudo nmcli connection add type ethernet \
    ifname enx020000000001 \
    con-name phonostethos-usb \
    ipv4.method manual \
    ipv4.addresses 10.42.0.10/24 \
    ipv4.routes "10.42.0.3/32" \
    ipv6.method ignore \
    connection.autoconnect yes

# verificar
nmcli connection show phonostethos-usb
```

### `/etc/hosts` do PC
```bash
sudo sed -i '/kalam/d; /paran/d' /etc/hosts
echo "10.42.0.2    kalam"  | sudo tee -a /etc/hosts
echo "10.42.0.3    paran"  | sudo tee -a /etc/hosts
grep 10.42 /etc/hosts
```

---

## 📁 ESTRUTURA DO OVERLAY (no PC)

```
board/phonostethos/
├── cmdline.txt                               ← boot/cmdline.txt
├── config.txt                                ← boot/config.txt
├── post-build.sh                             ← corrige perms das chaves SSH
└── rootfs_overlay/
    ├── etc/
    │   ├── asound.conf                       ← default = mic USB + jack Pi
    │   ├── dhcpcd.conf                       ← denyinterfaces usb0 eth0
    │   ├── network/interfaces                ← IPs + rotas /32
    │   ├── init.d/S11gadget                  ← modprobe g_ether no boot
    │   └── ssh/
    │       ├── ssh_host_ed25519_key          ← chaves FIXAS (600)
    │       ├── ssh_host_ed25519_key.pub      ← (644)
    │       ├── ssh_host_rsa_key              ← (600)
    │       ├── ssh_host_rsa_key.pub          ← (644)
    │       └── sshd_config                   ← PermitRootLogin yes
    └── usr/bin/                              ← (só quando fizeres phonostethos)
```

---

## 🗺️ MAPA DE IPS E NOMES

| Interface | Onde | IP | Nome (`/etc/hosts` do PC) |
|---|---|---|---|
| `usb0` | Pi | `10.42.0.3` | `paran` |
| `enx020000000001` | PC | `10.42.0.10` | (auto via NM) |
| `eth0` | Pi | `10.42.0.2` | `kalam` |
| `enp1s0` | PC | `10.42.0.1` | (auto) |

**Fingerprint SSH esperada:** `SHA256:ZBNJHkTVUHH07wFg630gzDMF3dtNS6+k52gZ901kY1U`

---

## ⚠️ REGRAS DE OURO

1. **Nunca tirares o cartão sem `sync; sync`**
2. **Nunca cortares power na Pi sem `sync; sync; poweroff`**
3. **Verificar hash depois do `dd`** (imagem vs cartão)
4. **Sempre `ssh-keyscan -lf -` depois de flashar** para confirmar `ZBNJHkTV...`
5. **Nunca editar `output/target/`** — só o overlay (`board/phonostethos/`)
6. **Mudanças no `.config`** → `rm -rf output/target` + `find output/build -name .stamp_target_installed -delete` + `make`

---

## 🛑 DESLIGAR A PI EM SEGURANÇA

```bash
# na Pi (via ssh)
sync
sync
poweroff
```

Espera 10 s até o LED verde parar de piscar, **só depois** tira o cabo.
