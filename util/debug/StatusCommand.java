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
			System.out.println(dbg);
		else
			System.out.println("No file loaded.");
		return data;
	}
}
