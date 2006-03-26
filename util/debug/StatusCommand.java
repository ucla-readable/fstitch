//import java.util.*;

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
				for(int i = 0; i != args.length; i++)
				{
					int number = SystemState.unhex(args[i]);
					Chdesc chdesc = state.lookupChdesc(number);
					if(chdesc != null)
						System.out.println("Chdesc " + SystemState.hex(number) + " was created by opcode " + chdesc.opcode);
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
}
