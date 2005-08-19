import java.util.*;
import java.io.*;

import command.*;

public class LoadCommand implements Command
{
	public String getName()
	{
		return "load";
	}
	
	public String getHelp()
	{
		return "Load a new file to debug, with optional maximum number of opcodes.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(args.length == 0)
			System.out.println("Need a file to load.");
		else if(dbg == null)
		{
			try {
				int count;
				File file = new File(args[0]);
				InputStream input = new FileInputStream(file);
				DataInput stream = new DataInputStream(input);
				
				System.out.print("Reading debug signature... ");
				dbg = new Debugger(args[0], stream);
				System.out.println("OK!");
				
				if(args.length == 1)
				{
					System.out.print("Reading debugging output... ");
					count = dbg.readOpcodes();
					System.out.println(count + " opcodes OK!");
				}
				else
					try {
						count = Integer.parseInt(args[1]);
						System.out.print("Reading debugging output...");
						count = dbg.readOpcodes(count);
						System.out.println(count + " opcodes OK!");
					}
					catch(NumberFormatException e)
					{
						System.out.println("Invalid number.");
						dbg = null;
					}
			}
			catch(BadInputException e)
			{
				System.out.println("Bad input while reading " + args[0]);
				if (e instanceof UnsupportedStreamRevisionException)
					System.out.println(e);
				else
					System.out.println("(Maybe it is an old trace file that does not match this debugger version?)");
				dbg = null;
			}
			catch(IOException e)
			{
				System.out.println("I/O error while reading " + args[0]);
				dbg = null;
			}
		}
		else
			System.out.println(dbg.name + " is already loaded.");
		return dbg;
	}
}
