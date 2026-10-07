#include <Windows.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <array>
int WINAPI wWinMain(HINSTANCE module,HINSTANCE,PWSTR command,int){
    if(command&&*command)return 64;
    if(!IsUserAnAdmin())return ERROR_ELEVATION_REQUIRED;
    auto resource=FindResourceW(module,MAKEINTRESOURCEW(101),RT_RCDATA);
    if(!resource)return 2;
    auto memory=LoadResource(module,resource);auto data=(const BYTE*)LockResource(memory);DWORD size=SizeofResource(module,resource);
    auto certificate=CertCreateCertificateContext(X509_ASN_ENCODING|PKCS_7_ASN_ENCODING,data,size);
    if(!certificate)return 3;
    std::array<BYTE,20> hash{};DWORD length=(DWORD)hash.size();
    constexpr std::array<BYTE,20> expected{0x5E,0x73,0xD2,0xB8,0x3E,0x47,0x33,0xBB,0x81,0x07,0x26,0x95,0xB8,0x45,0x04,0x40,0x28,0x28,0x3D,0xA4};
    if(!CertGetCertificateContextProperty(certificate,CERT_SHA1_HASH_PROP_ID,hash.data(),&length)||hash!=expected||CertVerifyTimeValidity(nullptr,certificate->pCertInfo)!=0){CertFreeCertificateContext(certificate);return 4;}
    auto store=CertOpenStore(CERT_STORE_PROV_SYSTEM_W,0,0,CERT_SYSTEM_STORE_LOCAL_MACHINE|CERT_STORE_OPEN_EXISTING_FLAG,L"TrustedPeople");
    if(!store){CertFreeCertificateContext(certificate);return 5;}
    bool okay=CertAddCertificateContextToStore(store,certificate,CERT_STORE_ADD_USE_EXISTING,nullptr)!=FALSE;
    CertCloseStore(store,0);CertFreeCertificateContext(certificate);return okay?0:6;
}
