import java.io.DataInput;
import java.io.IOException;

public class ChdescAlterModule extends Module
{
	public ChdescAlterModule(DataInput input) throws BadInputException, IOException
	{
		super(input, KDB_MODULE_CHDESC_ALTER);
		
		addFactory(ChdescCreateNoop.getFactory(input));
		addFactory(ChdescCreateBit.getFactory(input));
		addFactory(ChdescCreateByte.getFactory(input));
	}
}
