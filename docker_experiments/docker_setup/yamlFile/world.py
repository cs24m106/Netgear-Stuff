
#Program to read a file on input

import sys

if len(sys.argv) < 1:
	print("Enter one argument")
	exit()

filename = sys.argv[1]

#filename = input("Enter the file name: ")
print("File name: ",filename)

#choice = int(input("Enter choice\n\t1)read\n\t2)write\nchoice: "))

fptr = open(filename, "r")
content = fptr.read()
print("The contents of ",fptr.name, " are: ")
#print(content)
alpha = 0
digit = 0
symbol = 0

for i in content:
	if i.isalpha():
		alpha += 1
	elif i.isdigit():
		digit += 1
	else:
		symbol += 1

fptr2 = open("PythonReport.txt", "w")
fptr2.write("Alphabets: "+str(alpha)+"\n")
fptr2.write("Digits: "+str(digit)+"\n")
fptr2.write("Symbols: "+str(symbol)+"\n")

fptr2.close()
fptr.close()


