//import java.util.*;
import java.util.Iterator;

import command.*;

public class StatusCommand implements Command
{
	public String getName()
	{
		return "status";
	}
	
	public String getHelp()
	{
		return "Displays system state status.";
	}
	
	public Object runCommand(String args[], Object data, CommandInterpreter interpreter) throws CommandException
	{
		Debugger dbg = (Debugger) data;
		if(dbg != null)
		{
			if(args.length > 0)
			{
				/* display status of chdescs */
				SystemState state = dbg.getState();
				int i = 0;
				boolean verbose = false;
				
				if(args[0].equals("-v"))
				{
					verbose = true;
					i++;
				}
				
				for(; i != args.length; i++)
				{
					int number;
					try {
						number = SystemState.unhex(args[i]);
					}
					catch(NumberFormatException e)
					{
						System.out.println("Invalid number: " + e);
						continue;
					}
					
					Chdesc chdesc = state.lookupChdesc(number);
					if(chdesc != null)
					{
						System.out.println("Chdesc " + SystemState.hex(chdesc.address) + " was created by opcode " + chdesc.opcode);
						if(verbose)
							printChdesc(state, chdesc);
					}
					else
						System.out.println("No such chdesc: " + SystemState.hex(number));
				}
			}
			else
				System.out.println(dbg);
		}
		else
			System.out.println("No file loaded.");
		return data;
	}

	private void printChdesc(SystemState state, Chdesc chdesc)
	{
		System.out.println(ChdescTypeToString(state, chdesc));
		
		if(chdesc.getLabel() != null)
			System.out.println("Label = \"" + chdesc.getLabel() + "\"");
		
		System.out.print("block address = " + SystemState.hex(chdesc.getBlock()));
		Bdesc bdesc = state.lookupBdesc(chdesc.getBlock());
		if(bdesc != null)
			System.out.print(", number = " + bdesc.number);
		System.out.println();
		
		System.out.print("owner address = " + SystemState.hex(chdesc.getOwner()));
		String bdName = state.getBdName(chdesc.getOwner());
		if(bdName != null)
			System.out.print(", name = " + bdName);
		System.out.println();
		
		System.out.println("Flags: " + chdesc.renderFlags(chdesc.getFlags()));
		
		System.out.println("Afters:");
		for(Iterator it = chdesc.getAfters(); it.hasNext();)
			printChdescBrief(state, (Chdesc) it.next());
		
		System.out.println("Befores:");
		for(Iterator it = chdesc.getBefores(); it.hasNext();)
			printChdescBrief(state, (Chdesc) it.next());
	}
	
	private void printChdescBrief(SystemState state, Chdesc chdesc)
	{
		int nafters = 0, nbefores = 0;
		for(Iterator it = chdesc.getAfters(); it.hasNext(); it.next())
			nafters++;
		for(Iterator it = chdesc.getBefores(); it.hasNext(); it.next())
			nbefores++;
		System.out.print(" " + SystemState.hex(chdesc.address));
		System.out.print(", " + ChdescTypeToString(state, chdesc));
		System.out.print(", nafters " + nafters);
		System.out.println(", nbefores " + nbefores);
	}
	
	private String ChdescTypeToString(SystemState state, Chdesc chdesc)
	{
		String block = BlockToString(state, chdesc.getBlock());
		String bs = "";
		if(!block.equals(""))
			bs = ", block " + block;
		switch(chdesc.getType())
		{
			case Chdesc.TYPE_NOOP:
				return "NOOP" + bs;
			case Chdesc.TYPE_BIT:
				return "BIT" +  bs + ", offset " + chdesc.getOffset() + ", xor " + SystemState.hex(chdesc.getXor());
			case Chdesc.TYPE_BYTE:
				return "BYTE" + bs + ", offset " + chdesc.getOffset() + ", length " + chdesc.getLength();
			case Chdesc.TYPE_DESTROY:
				return "DESTROY" + bs;
			case Chdesc.TYPE_DANGLING:
				return "DANGLING" + bs;
			default:
				return "Unknown type " + chdesc.getType() + bs;
		}
	}

	private String BlockToString(SystemState state, int block)
	{
		if(block == 0)
			return "";
		Bdesc bdesc = state.lookupBdesc(block);
		if(bdesc == null)
			return SystemState.hex(block);
		return "#" + bdesc.number;
	}
}
