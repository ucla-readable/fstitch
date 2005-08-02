import java.io.DataInput;
import java.io.IOException;

public class InfoModule extends Module
{
	public InfoModule(DataInput input) throws BadInputException, IOException
	{
		super(input, KDB_MODULE_INFO);
		
		addFactory(InfoMark.getFactory(input));
	}
}
