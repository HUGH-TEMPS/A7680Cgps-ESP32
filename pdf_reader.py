import sys
import subprocess
try:
    import pypdf
except ImportError:
    subprocess.check_call([sys.executable, '-m', 'pip', 'install', 'pypdf'])
    import pypdf

try:
    reader = pypdf.PdfReader('D:\\GPS\\A76XX_Series_AT_Command_Manual_V1.12.pdf')
    
    ranges = [
        (71, 86), (88, 108), (110, 154), (260, 272), (274, 290), (292, 309),
        (311, 314), (316, 321), (322, 363), (365, 380), (381, 405), (407, 433),
        (436, 463), (464, 471), (489, 492), (494, 497), (499, 518), (520, 531),
        (603, 605), (611, 618), (620, 627), (628, 640), (641, 648), (650, 665),
        (666, 667), (669, 675), (676, 679), (681, 682)
    ]

    for start, end in ranges:
        filename = f"{start} to {end}.txt"
        with open(filename, 'w', encoding='utf-8') as f:
            for i in range(start - 1, min(end, len(reader.pages))):
                text = reader.pages[i].extract_text()
                if text:
                    f.write(f'--- PAGE {i+1} ---\n')
                    f.write(text + '\n')
        print(f"Generated {filename}")
        
    print("DONE extracting all topics.")
except Exception as e:
    print("ERROR:", e)
